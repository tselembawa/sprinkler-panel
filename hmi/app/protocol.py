"""Shared definitions between the HMI (Raspberry Pi) and the controller.

These mirror the data model in the specification ("Data model & protocol" tab).
Both the simulated controller and the future serial link use them, so the
HMI never needs to know which one it is talking to.
"""

import struct
import zlib

PROTOCOL_VERSION = 1

CATEGORIES = ("A", "B", "DRIFT", "FAULT")

DEVICE_TYPES = (
    "SERVICEVENTIL",
    "UNDERSEKTIONSVENTIL",
    "LARMVENTIL",
    "PRESSOSTAT",
    "FLODESVAKT",
    "PUMP_RUN",
    "PUMP_FAULT",
    "TEMP",
    "GENERIC",
)

# Decided: only fire alarms (water flow) latch. Everything else resets itself
# when the cause disappears.
LATCHING_DEVICE_TYPES = frozenset({"PRESSOSTAT", "FLODESVAKT"})

# Decided: technical upper limit for any alarm delay.
MAX_DELAY_S = 60

CONTACTS = ("NO", "NC")

# Line conditions as reported by an I/O module for one input.
CONDITIONS = ("NORMAL", "ACTIVE", "OPEN", "SHORT")

# Point states (see the state machine diagram in the specification).
STATES = ("NORMAL", "PENDING", "ALARM", "ACKED", "LATCHED")

LIMITS = {
    "point_id": (1, 999),
    "line": (1, 4),
    "module": (1, 247),
    "channel": (1, 4),
}

LCD_LINE_LEN = 20

# Points the controller can hold (ALM_MAX_POINTS).
MAX_POINTS = 256

# Fields the controller stores. The full name, graphics and users stay on the Pi.
CONTROLLER_FIELDS = (
    "id",
    "line",
    "module",
    "channel",
    "lcd1",
    "lcd2",
    "category",
    "device_type",
    "contact",
    "delay_s",
    "section",
    "enabled",
)


def controller_view(point: dict) -> dict:
    """Return only the fields of a point that the controller stores."""
    out = {}
    for key in CONTROLLER_FIELDS:
        value = point.get(key)
        if key == "enabled":
            value = bool(value)
        out[key] = value
    return out


def lcd_bytes(text) -> bytes:
    """An LCD line as the controller stores it: Latin-1 (Swedish letters fit), 20 bytes, 0-padded."""
    raw = str(text or "").encode("latin-1", errors="replace")[:LCD_LINE_LEN]
    return raw.ljust(LCD_LINE_LEN, b"\0")


def _index(values, value) -> int:
    return values.index(value) if value in values else 0xFF


def point_record(point: dict) -> bytes:
    """The 52-byte binary form of a point (CFG_PUT record, docs/pi-link.md).

    The same bytes are used for the configuration CRC on both sides.
    """
    p = controller_view(point)
    return struct.pack(
        "<HBBBBBBBBH20s20s",
        int(p["id"] or 0) & 0xFFFF,
        int(p["line"] or 0) & 0xFF,
        int(p["module"] or 0) & 0xFF,
        int(p["channel"] or 0) & 0xFF,
        _index(CATEGORIES, p["category"]),
        _index(DEVICE_TYPES, p["device_type"]),
        1 if p["contact"] == "NC" else 0,
        int(p["delay_s"] or 0) & 0xFF,
        1 if p["enabled"] else 0,
        int(p["section"] or 0) & 0xFFFF,
        lcd_bytes(p["lcd1"]),
        lcd_bytes(p["lcd2"]),
    )


def config_crc(points: list[dict]) -> int:
    """CRC-32 (zlib) over the binary point records sorted by id.

    The Pi computes it before upload and compares it with what the controller
    reports after activation (the controller computes the same: alm_config_crc).
    Only when they match does the UI say "saved".
    """
    blob = b"".join(point_record(p) for p in sorted(points, key=lambda p: p.get("id") or 0))
    return zlib.crc32(blob) & 0xFFFFFFFF


def validate_points(points: list[dict]) -> list[dict]:
    """Configuration check, the same rules as the controller (alm_validate)."""
    errors = []
    seen_ids: set[int] = set()
    seen_addr: dict[tuple, int] = {}

    def err(pid, fld, code):
        errors.append({"point_id": pid, "field": fld, "code": code})

    for p in points:
        pid = p.get("id")
        for fld, (lo, hi) in LIMITS.items():
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
        if p.get("category") not in CATEGORIES:
            err(pid, "category", "INVALID")
        if p.get("device_type") not in DEVICE_TYPES:
            err(pid, "device_type", "INVALID")
        if p.get("contact") not in CONTACTS:
            err(pid, "contact", "INVALID")
        d = p.get("delay_s")
        if not isinstance(d, int) or not 0 <= d <= MAX_DELAY_S:
            err(pid, "delay_s", "DELAY_NOT_ALLOWED")
        for fld in ("lcd1", "lcd2"):
            if len(p.get(fld) or "") > LCD_LINE_LEN:
                err(pid, fld, "TOO_LONG")
    if len(points) > MAX_POINTS:
        err(None, "id", "TOO_MANY")
    return errors


def is_latching(device_type: str) -> bool:
    return device_type in LATCHING_DEVICE_TYPES
