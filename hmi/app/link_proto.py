"""Raspberry Pi <-> Main Controller frame protocol v1 (docs/pi-link.md).

Python twin of firmware/common/pi_link_proto.h and cobs.c: both sides must agree byte for byte.
Pure functions only - the serial port lives in serial_controller.py.
"""

from __future__ import annotations

import binascii
import struct

from . import protocol as P

VERSION = 1
HEADER_LEN = 6
MAX_PAYLOAD = 512

# message types
PING = 0x01
GET_STATUS = 0x02
GET_POINTS = 0x04
GET_MODULES = 0x05
GET_EVENTS = 0x06
CFG_BEGIN = 0x10
CFG_PUT = 0x11
CFG_END = 0x12
CFG_ACTIVATE = 0x13
CFG_INFO = 0x14
CMD_ACK = 0x20
CMD_SILENCE = 0x21
CMD_RESET = 0x22
CMD_DISABLE = 0x23
CMD_SERVICE = 0x25
SET_TIME = 0x30
SET_LANGUAGE = 0x31
DEV_SET_KEY = 0x40
LOG = 0x60
NACK = 0x7F

NACK_CODES = {
    1: "UNKNOWN_TYPE",
    2: "BAD_LENGTH",
    3: "ACCESS_DENIED",
    4: "BAD_DURATION",
    5: "UNKNOWN_POINT",
    6: "BAD_TIME",
    7: "CFG_SEQUENCE",
    8: "CFG_TOO_MANY",
    9: "CFG_CRC_MISMATCH",
    10: "BAD_VERSION",
    11: "BAD_LANGUAGE",
}

CFG_RECORDS_PER_FRAME = 9   # 9 x 52 = 468 bytes <= 512
POINT_REC = 10
EVENT_REC = 21
MODULE_REC = 12
STATUS_LEN = 43

EVENT_TYPES = (
    "STARTUP", "CONFIG_ACTIVATED", "DELAY_STARTED", "DELAY_CANCELLED", "ALARM_ON", "ALARM_CLEARED",
    "LINE_OPEN", "LINE_SHORT", "LINE_OK", "MODULE_LOST", "MODULE_BACK", "ACK", "RESET", "SILENCE",
    "UNSILENCE", "RESOUND", "SERVICE_ON", "SERVICE_OFF", "POINT_DISABLED", "POINT_ENABLED",
    "TIME_SET", "HMI_LOST", "HMI_BACK",
)
SOURCE_KINDS = ("SYSTEM", "POINT", "MODULE")
FAULTS = (None, "OPEN", "SHORT", "MODULE_LOST")
MODULE_STATES = ("START", "RUN", "LOST", "REJECTED")
# alm_cfg_field_t / alm_cfg_err_code_t
CFG_FIELDS = ("id", "line", "module", "channel", "category", "device_type", "delay_s", "lcd1", "lcd2")
CFG_ERRORS = {1: "OUT_OF_RANGE", 2: "DUPLICATE_ID", 3: "DUPLICATE_ADDRESS", 4: "INVALID",
              5: "DELAY_NOT_ALLOWED", 6: "TOO_LONG", 7: "TOO_MANY"}
DISPLAY = {"A": "A", "B": "B", "C": "C", "D": "DRIFT", "F": "FAULT"}
# language numbers on the link (and of the controller's LCD texts)
LANGS = ("sv", "en", "fi", "no", "da", "el", "ru")


class ProtocolError(Exception):
    """A frame that cannot be understood (bad CRC, bad COBS, wrong length)."""


# ------------------------------------------------------------------ framing
def crc16(data: bytes) -> int:
    """CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF."""
    return binascii.crc_hqx(data, 0xFFFF)


def cobs_encode(data: bytes) -> bytes:
    out = bytearray()
    block = bytearray()
    for b in data:
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(b)
            if len(block) == 254:
                out.append(255)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        code = data[i]
        if code == 0:
            raise ProtocolError("bad COBS")
        i += 1
        end = i + code - 1
        if end > n:
            raise ProtocolError("bad COBS")
        chunk = data[i:end]
        if 0 in chunk:
            raise ProtocolError("bad COBS")
        out += chunk
        i = end
        if code < 255 and i < n:
            out.append(0)
    return bytes(out)


def build_frame(mtype: int, seq: int, payload: bytes = b"") -> bytes:
    """Encoded frame ready for the wire, including the final 0x00."""
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too long")
    raw = struct.pack("<BBBBH", VERSION, mtype, seq & 0xFF, 0, len(payload)) + payload
    raw += struct.pack("<H", crc16(raw))
    return cobs_encode(raw) + b"\0"


def parse_frame(encoded: bytes) -> tuple[int, int, int, bytes]:
    """Encoded frame without the 0x00 -> (version, type, seq, payload)."""
    raw = cobs_decode(encoded)
    if len(raw) < HEADER_LEN + 2:
        raise ProtocolError("short frame")
    ver, mtype, seq, _flags, length = struct.unpack_from("<BBBBH", raw)
    if len(raw) != HEADER_LEN + length + 2:
        raise ProtocolError("length mismatch")
    (crc,) = struct.unpack_from("<H", raw, HEADER_LEN + length)
    if crc != crc16(raw[: HEADER_LEN + length]):
        raise ProtocolError("bad CRC")
    return ver, mtype, seq, raw[HEADER_LEN: HEADER_LEN + length]


# ------------------------------------------------------------------ records
def _state(v: int) -> str:
    return P.STATES[v] if v < len(P.STATES) else str(v)


def parse_status(p: bytes) -> dict:
    if len(p) < STATUS_LEN:
        raise ProtocolError("short status")
    a, b, c, drift, fault, disabled = struct.unpack_from("<6H", p, 0)
    logical, relays, led_b, sounders, f1, f2 = struct.unpack_from("<6B", p, 12)
    svc_left, ctime, uptime, cfg_ver, cfg_crc, last_seq = struct.unpack_from("<6I", p, 18)
    nlost = p[42]
    lost = [[p[STATUS_LEN + 2 * i], p[STATUS_LEN + 2 * i + 1]] for i in range(nlost)
            if STATUS_LEN + 2 * i + 1 < len(p)]

    def rel(bits):
        return {"K1_A": bool(bits & 1), "K2_B": bool(bits & 2),
                "K3_FAULT_OK": bool(bits & 4), "K4_C": bool(bits & 8)}

    service = bool(f1 & 0x04)
    return {
        "counts": {"A": a, "B": b, "C": c, "DRIFT": drift, "FAULT": fault, "DISABLED": disabled},
        "relays_logical": rel(logical),
        "relays": rel(relays),
        "transmission_blocked": bool(f1 & 0x01),
        "leds": {
            "red_a": bool(f1 & 0x20),
            "yellow_b": ("off", "steady", "flash")[led_b] if led_b < 3 else "off",
            "yellow_fault": bool(f1 & 0x40),
            "green_drift": bool(f1 & 0x80),
            "green_power": bool(f2 & 0x01),
            "yellow_service": bool(f2 & 0x02),
        },
        "buzzer": bool(f1 & 0x10),
        "sounders": ("off", "sounding", "silenced")[sounders] if sounders < 3 else "off",
        "silenced": bool(f1 & 0x02),
        "service": {"active": service, "remaining_s": svc_left if service else 0},
        "key_tech": bool(f1 & 0x08),
        "hmi_lost": bool(f2 & 0x04),
        "language": LANGS[(f2 >> 4) & 7] if ((f2 >> 4) & 7) < len(LANGS) else "sv",
        "time": ctime,
        "uptime_s": uptime,
        "config_version": cfg_ver,
        "config_crc": cfg_crc,
        "last_event_seq": last_seq,
        "lost_modules": sorted(lost),
    }


def parse_points(p: bytes) -> tuple[int, list[dict]]:
    total, count = struct.unpack_from("<HB", p, 0)
    out = []
    for i in range(count):
        o = 3 + POINT_REC * i
        pid, cat, st, acked, fault, enabled, disp, cond, left = struct.unpack_from("<H8B", p, o)
        out.append({
            "id": pid,
            "state": _state(st),
            "acked": bool(acked),
            "fault": FAULTS[fault] if fault < len(FAULTS) else "OPEN",
            "enabled": bool(enabled),
            "category": P.CATEGORIES[cat] if cat < len(P.CATEGORIES) else str(cat),
            "display_category": DISPLAY.get(chr(disp), chr(disp)),
            "condition": P.CONDITIONS[cond] if cond < len(P.CONDITIONS) else "NORMAL",
            "pending_left_s": None if left == 0xFF else left,
        })
    return total, out


def _event_value(etype: str, which: str, v: int) -> str:
    if etype in ("DELAY_STARTED", "DELAY_CANCELLED", "ALARM_ON", "ALARM_CLEARED"):
        return _state(v)
    if etype == "TIME_SET":
        return str(v & 0xFFFFFFFF)
    if which == "new":
        if etype == "SERVICE_ON":
            return str(v)
        if etype == "SERVICE_OFF":
            return "manual" if v else "timeout"
        if etype == "CONFIG_ACTIVATED":
            return str(v)
    return ""


def parse_events(p: bytes) -> list[dict]:
    count = p[0]
    out = []
    for i in range(count):
        seq, t, etype, kind, user, sid, old, new = struct.unpack_from("<IIBBBHii", p, 1 + EVENT_REC * i)
        name = EVENT_TYPES[etype] if etype < len(EVENT_TYPES) else f"TYPE_{etype}"
        out.append({
            "seq": seq,
            "time": t,
            "type": name,
            "source_kind": SOURCE_KINDS[kind] if kind < len(SOURCE_KINDS) else "SYSTEM",
            "source_id": sid,
            "old": _event_value(name, "old", old),
            "new": _event_value(name, "new", new),
            "user": user,
        })
    return out


def parse_modules(p: bytes) -> list[dict]:
    out = []
    for i in range(p[0]):
        addr, line, st, inputs, status, mv, fw, fails = struct.unpack_from("<BBBHHHHB", p, 1 + MODULE_REC * i)
        out.append({"address": addr, "line": line,
                    "state": MODULE_STATES[st] if st < len(MODULE_STATES) else str(st),
                    "inputs": inputs, "status": status, "supply_mv": mv,
                    "firmware": f"{fw >> 8}.{fw & 0xFF}", "fails": fails})
    return out


def parse_cfg_errors(p: bytes) -> list[dict]:
    (n,) = struct.unpack_from("<H", p, 0)
    out = []
    for i in range(min(n, (len(p) - 2) // 4)):
        pid, fld, code = struct.unpack_from("<HBB", p, 2 + 4 * i)
        out.append({"point_id": pid or None,
                    "field": CFG_FIELDS[fld] if fld < len(CFG_FIELDS) else str(fld),
                    "code": CFG_ERRORS.get(code, str(code))})
    return out
