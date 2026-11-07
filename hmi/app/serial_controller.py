"""The real Main Controller over the serial link (docs/pi-link.md).

SerialController has the same public methods as SimController, so the rest of the HMI
does not know which one it talks to. Select it with

    SPRINKLER_CONTROLLER=serial            (port found automatically: the Nucleo's ST-LINK)
    SPRINKLER_CONTROLLER=serial:/dev/ttyACM0
    SPRINKLER_CONTROLLER=serial:COM5       (Windows)

Event numbers: the controller restarts its event numbering at every start-up (until it
gets FRAM). The HMI keeps one continuous numbering in its database, so this class adds
an offset and notices controller restarts (uptime going back).
"""

from __future__ import annotations

import json
import struct
import threading
import time
from collections import deque

from . import link_proto as L
from . import protocol as P
from .sim_controller import CommandError, ControllerUnavailable

ST_LINK_VID = 0x0483       # STMicroelectronics (the Nucleo's ST-LINK virtual COM port)
CLOCK_VALID_FROM = 1577836800   # 2020-01-01: below this the controller clock was never set


def find_port() -> str | None:
    """The serial port of the controller: the ST-LINK of the Nucleo, else the first ttyACM."""
    try:
        from serial.tools import list_ports
    except ImportError:
        return None
    ports = list(list_ports.comports())
    for p in ports:
        if p.vid == ST_LINK_VID or "STLink" in (p.description or "") or "STM" in (p.manufacturer or ""):
            return p.device
    for p in ports:
        if "ttyACM" in p.device:
            return p.device
    return None


def open_serial(port: str | None):
    import serial  # pyserial

    port = port or find_port()
    if not port:
        raise ControllerUnavailable("no serial port found")
    return serial.Serial(port, 115200, timeout=0.02, write_timeout=1)


class SerialLink:
    """Request/answer over one serial port: framing, retries, sequence numbers."""

    TIMEOUT_S = 0.3
    RETRIES = 3
    REOPEN_EVERY_S = 2.0
    DOWN_RETRY_S = 1.0      # while the controller is silent: one quick try per second, no queue

    def __init__(self, opener, log=None, clock=time.monotonic):
        self._opener = opener          # () -> object with read(n), write(b), close()
        self._log = log or (lambda text: print("CONTROLLER:", text, flush=True))
        self._clock = clock
        self._port = None
        self._next_open = 0.0
        self._seq = 0
        self._rx = bytearray()
        self.lock = threading.RLock()
        self.logs: deque[str] = deque(maxlen=200)
        self.frames_bad = 0
        self.connected = False
        self._down_until = 0.0

    def close(self):
        with self.lock:
            self._drop()

    def _drop(self):
        if self._port is not None:
            try:
                self._port.close()
            except Exception:  # noqa: BLE001
                pass
        self._port = None
        self.connected = False
        self._rx.clear()

    def _ensure_open(self):
        if self._port is not None:
            return
        now = self._clock()
        if now < self._next_open:
            raise ControllerUnavailable("serial port not open")
        self._next_open = now + self.REOPEN_EVERY_S
        try:
            self._port = self._opener()
        except ControllerUnavailable:
            raise
        except Exception as e:  # noqa: BLE001 - SerialException, OSError ...
            raise ControllerUnavailable(f"cannot open serial port: {e}") from e

    def _frames(self, data: bytes):
        """Split received bytes into frames; yields parsed (type, seq, payload)."""
        for b in data:
            if b != 0:
                self._rx.append(b)
                if len(self._rx) > 2 * L.MAX_PAYLOAD + 64:
                    self._rx.clear()
                    self.frames_bad += 1
                continue
            if not self._rx:
                continue
            raw = bytes(self._rx)
            self._rx.clear()
            try:
                ver, mtype, seq, payload = L.parse_frame(raw)
            except L.ProtocolError:
                self.frames_bad += 1
                continue
            if mtype == L.LOG:
                text = payload.decode("latin-1", errors="replace").rstrip()
                self.logs.append(text)
                self._log(text)
                continue
            yield mtype, seq, payload

    def request(self, mtype: int, payload: bytes = b"") -> bytes:
        with self.lock:
            if not self.connected and self._clock() < self._down_until:
                # it did not answer a moment ago: do not make every caller wait for timeouts
                raise ControllerUnavailable("no answer from the controller")
            self._ensure_open()
            self._seq = self._seq % 255 + 1          # 1..255, 0 is for LOG
            frame = L.build_frame(mtype, self._seq, payload)
            for _attempt in range(self.RETRIES):
                try:
                    self._port.write(frame)
                    deadline = self._clock() + self.TIMEOUT_S
                    while self._clock() < deadline:
                        data = self._port.read(256)
                        for rtype, rseq, rpayload in self._frames(data):
                            if rseq != self._seq:
                                continue      # a late answer to an earlier request
                            self.connected = True
                            if rtype == L.NACK:
                                code = L.NACK_CODES.get(rpayload[0] if rpayload else 0, "NACK")
                                raise CommandError(code, rpayload[1:].hex())
                            return rpayload
                except CommandError:
                    raise
                except Exception as e:  # noqa: BLE001 - port unplugged etc.
                    self._drop()
                    raise ControllerUnavailable(f"serial port error: {e}") from e
            self.connected = False
            self._down_until = self._clock() + self.DOWN_RETRY_S
            raise ControllerUnavailable("no answer from the controller")


class SerialController:
    STATUS_CACHE_S = 0.2
    POINTS_PER_REQUEST = 50
    EVENTS_PER_REQUEST = 24

    def __init__(self, link: SerialLink, store=None, start_seq: int = 0, wall=time.time):
        self._link = link
        self._store = store            # db with get_setting/set_setting, keeps the event offset
        self._wall = wall
        self._start_seq = start_seq
        self._offset: int | None = None   # HMI event seq = controller seq + offset
        self._prev: dict | None = None    # last raw status (restart detection)
        self._saved_at = 0.0
        self._cache: tuple[float, dict] | None = None
        self._fw = "?"
        self._lock = threading.RLock()

    # ------------------------------------------------------------- helpers
    def _req(self, mtype, payload=b""):
        return self._link.request(mtype, payload)

    def close(self):
        self._link.close()

    @property
    def logs(self) -> list[str]:
        return list(self._link.logs)

    def _load_saved(self) -> dict | None:
        if not self._store:
            return None
        try:
            return json.loads(self._store.get_setting("controller_link", "") or "null")
        except (ValueError, TypeError):
            return None

    def _save(self, raw: dict, force=False):
        now = self._wall()
        if not self._store or (not force and now - self._saved_at < 30):
            return
        self._saved_at = now
        self._store.set_setting("controller_link", json.dumps({
            "offset": self._offset, "uptime": raw["uptime_s"], "seq": raw["last_event_seq"], "wall": now}))

    def _track_restart(self, raw: dict):
        """Keep the HMI event numbering continuous across controller and HMI restarts."""
        if self._offset is None:
            saved = self._load_saved()
            same_boot = False
            if saved and saved.get("offset") is not None:
                elapsed = self._wall() - saved["wall"]
                expected = saved["uptime"] + elapsed
                same_boot = (raw["uptime_s"] >= saved["uptime"] and raw["last_event_seq"] >= saved["seq"]
                             and abs(raw["uptime_s"] - expected) < max(120, 0.05 * elapsed))
            self._offset = saved["offset"] if same_boot else self._start_seq
            self._save(raw, force=True)
        elif self._prev and (raw.get("clock_was_unset") or raw["uptime_s"] + 2 < self._prev["uptime_s"]
                             or raw["last_event_seq"] < self._prev["last_event_seq"]):
            # the controller restarted: its numbering began again from 0
            self._offset += self._prev["last_event_seq"]
            self._cache = None
            self._save(raw, force=True)
        else:
            self._save(raw)
        self._prev = raw

    def _raw_status(self) -> dict:
        raw = L.parse_status(self._req(L.GET_STATUS))
        if raw["time"] < CLOCK_VALID_FROM:
            # The controller has no battery clock yet: it gets the Pi's time after every start.
            # An unset clock also proves that the controller started again.
            try:
                self._req(L.SET_TIME, struct.pack("<IBB", int(self._wall()), 0, 1))
                raw = L.parse_status(self._req(L.GET_STATUS))
            except CommandError:
                pass
            raw["clock_was_unset"] = True
        if self._fw == "?":
            ping = self._req(L.PING)
            self._fw = f"{ping[1]}.{ping[2]}"
        return raw

    # ------------------------------------------------------------- status
    def get_status(self) -> dict:
        with self._lock:
            now = time.monotonic()
            if self._cache and now - self._cache[0] < self.STATUS_CACHE_S:
                return dict(self._cache[1])
            raw = self._raw_status()
            self._track_restart(raw)
            st = dict(raw)
            st.pop("clock_was_unset", None)
            st["protocol"] = L.VERSION
            st["firmware"] = self._fw
            st["last_event_seq"] = raw["last_event_seq"] + self._offset
            st["controller_link"] = True
            self._cache = (now, st)
            return dict(st)

    def get_points(self) -> list[dict]:
        out: list[dict] = []
        while True:
            total, batch = L.parse_points(self._req(
                L.GET_POINTS, struct.pack("<HB", len(out), self.POINTS_PER_REQUEST)))
            out += batch
            if not batch or len(out) >= total:
                return out

    def get_modules(self) -> list[dict]:
        return L.parse_modules(self._req(L.GET_MODULES))

    def get_events(self, from_seq: int = 0, max_count: int = 200) -> list[dict]:
        with self._lock:
            self.get_status()           # notices a controller restart before reading its events
            offset = self._offset
        ctrl_from = max(0, from_seq - offset)
        out: list[dict] = []
        while len(out) < max_count:
            batch = L.parse_events(self._req(
                L.GET_EVENTS, struct.pack("<IB", ctrl_from, min(self.EVENTS_PER_REQUEST, max_count - len(out)))))
            if not batch:
                break
            for e in batch:
                ctrl_from = e["seq"]
                e["seq"] += offset
                out.append(e)
        return out

    # ------------------------------------------------------------- configuration
    def config_info(self) -> dict:
        version, crc, points = struct.unpack("<IIH", self._req(L.CFG_INFO))
        return {"version": version, "crc": crc, "points": points}

    def validate(self, points: list[dict]) -> list[dict]:
        return P.validate_points(points)

    def upload_config(self, points: list[dict], user: int = 0) -> dict:
        """CFG_BEGIN, CFG_PUT x n, CFG_END (controller checks CRC and rules), CFG_ACTIVATE."""
        points = sorted((P.controller_view(p) for p in points), key=lambda p: p["id"] or 0)
        errors = P.validate_points(points)
        if errors:
            info = self.config_info()
            return {"ok": False, "errors": errors, "version": info["version"], "crc": info["crc"]}
        records = [P.point_record(p) for p in points]
        with self._link.lock:
            self._req(L.CFG_BEGIN, struct.pack("<H", len(records)))
            for i in range(0, len(records), L.CFG_RECORDS_PER_FRAME):
                self._req(L.CFG_PUT, b"".join(records[i:i + L.CFG_RECORDS_PER_FRAME]))
            errors = L.parse_cfg_errors(self._req(L.CFG_END, struct.pack("<I", P.config_crc(points))))
            if errors:
                info = self.config_info()
                return {"ok": False, "errors": errors, "version": info["version"], "crc": info["crc"]}
            version, crc = struct.unpack("<II", self._req(L.CFG_ACTIVATE, bytes([user & 0xFF])))
        self._cache = None
        return {"ok": True, "errors": [], "version": version, "crc": crc}

    # ------------------------------------------------------------- commands
    def _done(self, result):
        self._cache = None
        return result

    def cmd_ack(self, user: int = 0) -> dict:
        (n,) = struct.unpack("<H", self._req(L.CMD_ACK, bytes([user & 0xFF])))
        return self._done({"acknowledged": n})

    def cmd_reset(self, user: int = 0) -> dict:
        latched = {p["id"] for p in self.get_points() if p["state"] == "LATCHED"}
        p = self._req(L.CMD_RESET, bytes([user & 0xFF]))
        _reset, remaining = struct.unpack_from("<HH", p)
        remains = [struct.unpack_from("<H", p, 4 + 2 * i)[0] for i in range(min(remaining, (len(p) - 4) // 2))]
        now_normal = {q["id"] for q in self.get_points() if q["state"] == "NORMAL"}
        return self._done({"reset": sorted(latched & now_normal), "cause_remains": remains})

    def cmd_silence(self, silence: bool = True, user: int = 0) -> dict:
        p = self._req(L.CMD_SILENCE, bytes([user & 0xFF, 1 if silence else 0]))
        return self._done({"silenced": bool(p[0])})

    def cmd_service(self, minutes: int, user: int = 0) -> dict:
        if not isinstance(minutes, int) or not 0 <= minutes <= 480:
            raise CommandError("BAD_DURATION", str(minutes))
        p = self._req(L.CMD_SERVICE, struct.pack("<HB", minutes, user & 0xFF))
        return self._done({"service": bool(p[0])})

    def cmd_disable(self, point_id: int, disabled: bool, user: int = 0) -> dict:
        if not 0 <= point_id <= 0xFFFF:
            raise CommandError("UNKNOWN_POINT", str(point_id))
        p = self._req(L.CMD_DISABLE, struct.pack("<HBB", point_id, 1 if disabled else 0, user & 0xFF))
        return self._done({"point_id": point_id, "enabled": bool(p[0])})

    def set_time(self, unix_time: float, user: int = 0) -> dict:
        if not isinstance(unix_time, (int, float)) or not 946684800 <= unix_time <= 4102444800:
            raise CommandError("BAD_TIME", str(unix_time))
        (t,) = struct.unpack("<I", self._req(L.SET_TIME, struct.pack("<IBB", int(unix_time), user & 0xFF, 0)))
        return self._done({"time": t})

    # ------------------------------------------------------------- prototype only
    def dev_set_key(self, tech: bool):
        """Until the real key switch is wired to the controller, the HMI plays it."""
        self._req(L.DEV_SET_KEY, bytes([1 if tech else 0]))
        self._done(None)
