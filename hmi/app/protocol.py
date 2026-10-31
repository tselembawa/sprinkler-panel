"""Shared definitions between the HMI (Raspberry Pi) and the controller.

These mirror the data model in the specification ("Data model & protocol" tab).
Both the simulated controller and the future serial link use them, so the
HMI never needs to know which one it is talking to.
"""

import json
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


def config_crc(points: list[dict]) -> int:
    """CRC-32 over the canonical controller configuration.

    The Pi computes it before upload and compares it with what the controller
    reports after activation. Only when they match does the UI say "saved".
    """
    canon = sorted((controller_view(p) for p in points), key=lambda p: p["id"])
    blob = json.dumps(canon, sort_keys=True, ensure_ascii=False, separators=(",", ":"))
    return zlib.crc32(blob.encode("utf-8")) & 0xFFFFFFFF


def is_latching(device_type: str) -> bool:
    return device_type in LATCHING_DEVICE_TYPES
