"""Simulated Main Controller.

Runs inside the HMI process during development. It implements the same alarm
logic the real firmware will have (state machine, delays, latching, C-larm,
relays K1-K4, events with sequence numbers, staged configuration), so the UI
can be built and tested before any hardware exists.

Later the real controller is reached over UART through a class with the same
public methods (see controller_link.py).
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field

from . import protocol as P

ALARM_STATES = ("ALARM", "ACKED", "LATCHED")


@dataclass
class Runtime:
    state: str = "NORMAL"
    acked: bool = True
    fault: str | None = None  # "OPEN", "SHORT", "MODULE_LOST"
    pending_since: float | None = None
    changed_at: float = 0.0


@dataclass
class Event:
    seq: int
    time: float
    type: str
    source_kind: str
    source_id: int
    old: str = ""
    new: str = ""
    user: int = 0

    def as_dict(self) -> dict:
        return self.__dict__.copy()


class CommandError(Exception):
    """Equivalent of a NACK from the controller."""

    def __init__(self, code: str, detail: str = ""):
        super().__init__(code)
        self.code = code
        self.detail = detail


@dataclass
class _Config:
    version: int = 0
    crc: int = 0
    points: dict[int, dict] = field(default_factory=dict)


class SimController:
    EVENT_BUFFER = 2000

    def __init__(self, clock=time.monotonic, wall=time.time, start_seq: int = 0):
        self._clock = clock
        self._wall = wall
        self._lock = threading.RLock()
        self._cfg = _Config(crc=P.config_crc([]))
        self._rt: dict[int, Runtime] = {}
        self._inputs: dict[tuple[int, int, int], str] = {}
        self._lost_modules: set[tuple[int, int]] = set()
        self._events: list[Event] = []
        self._seq = start_seq  # the real controller keeps this in FRAM across restarts
        self._fault_unacked = False
        self._key_tech = False
        self._disabled: set[int] = set()  # runtime disablement (bortkoppling)
        self._time_offset = 0.0  # simulated RTC: wall clock + offset
        self._silenced = False  # "Tysta larmdon": external sounders silenced
        self._service_until: float | None = None  # service mode end (monotonic clock)
        self._started = clock()
        self._event("STARTUP", "SYSTEM", 0)

    # ------------------------------------------------------------------ events
    def _event(self, etype, kind, sid, old="", new="", user=0):
        self._seq += 1
        self._events.append(Event(self._seq, self.now(), etype, kind, sid, old, new, user))
        if len(self._events) > self.EVENT_BUFFER:
            del self._events[: len(self._events) - self.EVENT_BUFFER]

    # ------------------------------------------------------------------ clock
    def now(self) -> float:
        """Controller time (RTC), Unix seconds UTC."""
        return self._wall() + self._time_offset

    def set_time(self, unix_time: float, user: int = 0) -> dict:
        """SET_TIME: set the controller clock. Requires the key switch."""
        with self._lock:
            self._require_tech()
            if not isinstance(unix_time, (int, float)) or not 946684800 <= unix_time <= 4102444800:
                raise CommandError("BAD_TIME", str(unix_time))
            old = self.now()
            self._time_offset = float(unix_time) - self._wall()
            self._event("TIME_SET", "SYSTEM", 0, old=str(int(old)), new=str(int(unix_time)), user=user)
            return {"time": self.now()}

    # ------------------------------------------------------------ config flow
    def validate(self, points: list[dict]) -> list[dict]:
        errors = []
        seen_ids: set[int] = set()
        seen_addr: dict[tuple, int] = {}

        def err(pid, fld, code):
            errors.append({"point_id": pid, "field": fld, "code": code})

        for p in points:
            pid = p.get("id")
            for fld, (lo, hi) in P.LIMITS.items():
                key = "id" if fld == "point_id" else fld
                v = p.get(key)
                if not isinstance(v, int) or not lo <= v <= hi:
                    err(pid, key, "OUT_OF_RANGE")
            if pid in seen_ids:
                err(pid, "id", "DUPLICATE_ID")
            seen_ids.add(pid)
            addr = (p.get("line"), p.get("module"), p.get("channel"))
            if addr in seen_addr:
                err(pid, "channel", "DUPLICATE_ADDRESS")
            seen_addr[addr] = pid
            if p.get("category") not in P.CATEGORIES:
                err(pid, "category", "INVALID")
            if p.get("device_type") not in P.DEVICE_TYPES:
                err(pid, "device_type", "INVALID")
            if p.get("contact") not in P.CONTACTS:
                err(pid, "contact", "INVALID")
            d = p.get("delay_s")
            if not isinstance(d, int) or not 0 <= d <= P.MAX_DELAY_S:
                err(pid, "delay_s", "DELAY_NOT_ALLOWED")
            for fld in ("lcd1", "lcd2"):
                if len(p.get(fld) or "") > P.LCD_LINE_LEN:
                    err(pid, fld, "TOO_LONG")
        return errors

    def upload_config(self, points: list[dict], user: int = 0) -> dict:
        """CFG_BEGIN + CFG_PUT + CFG_END + CFG_ACTIVATE in one call.

        Returns {"ok": bool, "errors": [...], "version": int, "crc": int}.
        On errors the active configuration is left untouched.
        """
        points = [P.controller_view(p) for p in points]
        with self._lock:
            errors = self.validate(points)
            if errors:
                return {"ok": False, "errors": errors,
                        "version": self._cfg.version, "crc": self._cfg.crc}
            new = {p["id"]: p for p in points}
            old = self._cfg.points
            for pid, p in new.items():
                if pid not in old or old[pid] != p:
                    self._rt[pid] = Runtime(changed_at=self._clock())
            for pid in list(self._rt):
                if pid not in new:
                    del self._rt[pid]
            self._disabled &= set(new)
            self._cfg = _Config(self._cfg.version + 1, P.config_crc(points), new)
            self._event("CONFIG_ACTIVATED", "SYSTEM", 0, new=str(self._cfg.version), user=user)
            self._evaluate_all()
            return {"ok": True, "errors": [], "version": self._cfg.version, "crc": self._cfg.crc}

    def config_info(self) -> dict:
        with self._lock:
            return {"version": self._cfg.version, "crc": self._cfg.crc,
                    "points": len(self._cfg.points)}

    # ------------------------------------------------------------ simulation
    def sim_set_input(self, line: int, module: int, channel: int, condition: str):
        if condition not in P.CONDITIONS:
            raise CommandError("INVALID", condition)
        with self._lock:
            self._inputs[(line, module, channel)] = condition
            self._evaluate_all()

    def sim_set_module_lost(self, line: int, module: int, lost: bool):
        with self._lock:
            key = (line, module)
            sid = line * 1000 + module
            if lost and key not in self._lost_modules:
                self._lost_modules.add(key)
                self._event("MODULE_LOST", "MODULE", sid)
                self._fault_unacked = True
            elif not lost and key in self._lost_modules:
                self._lost_modules.discard(key)
                self._event("MODULE_BACK", "MODULE", sid)
            self._evaluate_all()

    def sim_set_key(self, tech: bool):
        with self._lock:
            self._key_tech = bool(tech)

    # ------------------------------------------------------------ main logic
    def tick(self):
        """Called periodically (e.g. every 100 ms) to run delay and service timers."""
        with self._lock:
            if self._service_until is not None and self._clock() >= self._service_until:
                self._service_until = None
                self._event("SERVICE_OFF", "SYSTEM", 0, new="timeout")
            self._evaluate_all()

    def _is_enabled(self, pid: int) -> bool:
        return bool(self._cfg.points[pid]["enabled"]) and pid not in self._disabled

    def _evaluate_all(self):
        for pid in sorted(self._cfg.points):
            self._evaluate(pid)

    def _set_state(self, pid, rt, new, etype, user=0):
        old = rt.state
        rt.state = new
        rt.changed_at = self._clock()
        if etype:
            self._event(etype, "POINT", pid, old, new, user)

    def _evaluate(self, pid: int):
        cfg = self._cfg.points[pid]
        rt = self._rt.setdefault(pid, Runtime())
        now = self._clock()

        if not self._is_enabled(pid):
            if rt.state != "NORMAL" or rt.fault:
                rt.state, rt.acked, rt.fault, rt.pending_since = "NORMAL", True, None, None
            return

        addr = (cfg["line"], cfg["module"], cfg["channel"])
        cond = self._inputs.get(addr, "NORMAL")

        # --- line supervision (independent of the alarm state) ---
        if (cfg["line"], cfg["module"]) in self._lost_modules:
            fault = "MODULE_LOST"
        elif cond in ("OPEN", "SHORT"):
            fault = cond
        else:
            fault = None
        if fault != rt.fault:
            if fault in ("OPEN", "SHORT"):
                self._event("LINE_" + fault, "POINT", pid)
                self._fault_unacked = True
            elif fault is None and rt.fault in ("OPEN", "SHORT"):
                self._event("LINE_OK", "POINT", pid)
            rt.fault = fault

        # A faulty line cannot report an alarm, but an existing alarm is kept.
        active = cond == "ACTIVE" and fault is None
        latching = P.is_latching(cfg["device_type"])
        auto_ack = cfg["category"] == "DRIFT"  # indications need no acknowledge
        st = rt.state

        if st == "NORMAL":
            if active:
                if cfg["delay_s"] > 0:
                    rt.pending_since = now
                    self._set_state(pid, rt, "PENDING", "DELAY_STARTED")
                else:
                    self._raise(pid, rt, auto_ack)
        elif st == "PENDING":
            if not active:
                rt.pending_since = None
                self._set_state(pid, rt, "NORMAL", "DELAY_CANCELLED")
            elif now - rt.pending_since >= cfg["delay_s"]:
                rt.pending_since = None
                self._raise(pid, rt, auto_ack)
        elif st in ("ALARM", "ACKED"):
            if not active and fault is None:
                if latching:
                    self._set_state(pid, rt, "LATCHED", "ALARM_CLEARED")
                else:
                    rt.acked = True
                    self._set_state(pid, rt, "NORMAL", "ALARM_CLEARED")
        elif st == "LATCHED":
            if active:
                self._raise(pid, rt, auto_ack)

    def _raise(self, pid, rt, auto_ack):
        rt.acked = auto_ack
        self._set_state(pid, rt, "ACKED" if auto_ack else "ALARM", "ALARM_ON")
        if self._silenced and self._cfg.points[pid]["category"] in ("A", "B"):
            self._silenced = False  # a new alarm always makes the sounders sound again
            self._event("RESOUND", "POINT", pid)

    # -------------------------------------------------------------- commands
    def _require_tech(self):
        if not self._key_tech:
            raise CommandError("ACCESS_DENIED", "Nyckelbrytaren måste vara i läge TEKNIKER")

    def cmd_ack(self, user: int = 0) -> dict:
        with self._lock:
            n = 0
            for pid, rt in self._rt.items():
                if rt.state == "ALARM":
                    self._set_state(pid, rt, "ACKED", None)
                    rt.acked = True
                    n += 1
                elif rt.state == "LATCHED" and not rt.acked:
                    rt.acked = True
                    n += 1
            if self._fault_unacked:
                self._fault_unacked = False
                n += 1
            self._event("ACK", "SYSTEM", 0, user=user)
            return {"acknowledged": n}

    def cmd_reset(self, user: int = 0) -> dict:
        with self._lock:
            self._require_tech()
            reset, remaining = [], []
            for pid, rt in self._rt.items():
                if rt.state == "LATCHED":
                    rt.acked = True
                    self._set_state(pid, rt, "NORMAL", None)
                    reset.append(pid)
                elif rt.state in ("ALARM", "ACKED") and P.is_latching(self._cfg.points[pid]["device_type"]):
                    remaining.append(pid)
            self._event("RESET", "SYSTEM", 0, user=user)
            if self._silenced and not any(rt.state in ALARM_STATES for rt in self._rt.values()):
                self._silenced = False
            return {"reset": reset, "cause_remains": remaining}

    def cmd_silence(self, silence: bool = True, user: int = 0) -> dict:
        """Tysta larmdon: silence (or re-activate) the external sounders. Requires the key."""
        with self._lock:
            self._require_tech()
            if silence != self._silenced:
                self._silenced = silence
                self._event("SILENCE" if silence else "UNSILENCE", "SYSTEM", 0, user=user)
            return {"silenced": self._silenced}

    def cmd_service(self, minutes: int, user: int = 0) -> dict:
        """Service mode for 1-480 minutes; 0 ends it. Requires the key."""
        with self._lock:
            self._require_tech()
            if not isinstance(minutes, int) or not 0 <= minutes <= 480:
                raise CommandError("BAD_DURATION", str(minutes))
            if minutes == 0:
                if self._service_until is not None:
                    self._service_until = None
                    self._event("SERVICE_OFF", "SYSTEM", 0, new="manual", user=user)
            else:
                self._service_until = self._clock() + minutes * 60
                self._event("SERVICE_ON", "SYSTEM", 0, new=str(minutes), user=user)
            return {"service": self._service_until is not None}

    def _sounders(self) -> str:
        """State of the external sounders. Until output rules exist: they follow any A-alarm."""
        fire = any(self._cfg.points[pid]["category"] == "A" and rt.state in ALARM_STATES
                   for pid, rt in self._rt.items() if pid in self._cfg.points and self._is_enabled(pid))
        if not fire:
            return "off"
        return "silenced" if self._silenced else "sounding"

    def cmd_disable(self, point_id: int, disabled: bool, user: int = 0) -> dict:
        with self._lock:
            self._require_tech()
            if point_id not in self._cfg.points:
                raise CommandError("UNKNOWN_POINT", str(point_id))
            if disabled:
                self._disabled.add(point_id)
            else:
                self._disabled.discard(point_id)
            self._event("POINT_DISABLED" if disabled else "POINT_ENABLED", "POINT", point_id, user=user)
            self._evaluate(point_id)
            return {"point_id": point_id, "enabled": not disabled}

    # ---------------------------------------------------------------- status
    def _display_category(self, cfg, rt):
        if cfg["category"] == "B" and rt.state == "ACKED":
            return "C"
        return cfg["category"]

    def get_status(self) -> dict:
        with self._lock:
            a_active = b_unacked = c_active = drift = disabled = 0
            fault_points = 0
            unacked = self._fault_unacked
            for pid, cfg in self._cfg.points.items():
                rt = self._rt.get(pid, Runtime())
                if not self._is_enabled(pid):
                    disabled += 1
                    continue
                cat = cfg["category"]
                if rt.fault:
                    fault_points += 1
                in_alarm = rt.state in ALARM_STATES
                if cat == "A" and in_alarm:
                    a_active += 1
                if cat == "B" and in_alarm and not rt.acked:
                    b_unacked += 1
                if cat == "B" and rt.state == "ACKED":
                    c_active += 1
                if cat == "DRIFT" and in_alarm:
                    drift += 1
                if cat == "FAULT" and in_alarm:
                    fault_points += 1
                if cat != "DRIFT" and in_alarm and not rt.acked:
                    unacked = True
            faults = fault_points + len(self._lost_modules)
            return {
                "protocol": P.PROTOCOL_VERSION,
                "firmware": "sim-0.1",
                "uptime_s": int(self._clock() - self._started),
                "config_version": self._cfg.version,
                "config_crc": self._cfg.crc,
                "last_event_seq": self._seq,
                "time": self.now(),
                "key_tech": self._key_tech,
                "counts": {"A": a_active, "B": b_unacked, "C": c_active,
                           "DRIFT": drift, "FAULT": faults, "DISABLED": disabled},
                "relays": {
                    "K1_A": a_active > 0,
                    "K2_B": b_unacked > 0,
                    "K3_FAULT_OK": faults == 0,  # fail-safe: energized when everything is OK
                    "K4_C": c_active > 0,
                },
                "leds": {
                    "red_a": a_active > 0,
                    "yellow_b": "flash" if b_unacked else ("steady" if c_active else "off"),
                    "yellow_fault": faults > 0,
                    "green_drift": drift > 0,   # an operational indication is active (pump running etc.)
                    "green_power": True,        # "Drift": the panel is running. Later: flashing on battery.
                    "yellow_service": self._service_until is not None,
                },
                "buzzer": unacked,
                "sounders": self._sounders(),
                "silenced": self._silenced,
                "service": {
                    "active": self._service_until is not None,
                    "remaining_s": int(max(0, self._service_until - self._clock())) if self._service_until else 0,
                },
                "lost_modules": sorted(self._lost_modules),
            }

    def get_points(self) -> list[dict]:
        with self._lock:
            out = []
            for pid in sorted(self._cfg.points):
                cfg = self._cfg.points[pid]
                rt = self._rt.get(pid, Runtime())
                addr = (cfg["line"], cfg["module"], cfg["channel"])
                out.append({
                    "id": pid,
                    "state": rt.state,
                    "acked": rt.acked,
                    "fault": rt.fault,
                    "enabled": self._is_enabled(pid),
                    "category": cfg["category"],
                    "display_category": self._display_category(cfg, rt),
                    "condition": self._inputs.get(addr, "NORMAL"),
                    "pending_left_s": (max(0, cfg["delay_s"] - (self._clock() - rt.pending_since))
                                       if rt.state == "PENDING" and rt.pending_since is not None else None),
                })
            return out

    def get_events(self, from_seq: int = 0, max_count: int = 200) -> list[dict]:
        with self._lock:
            return [e.as_dict() for e in self._events if e.seq > from_seq][:max_count]
