"""Frame protocol to the Main Controller (docs/pi-link.md): COBS, CRC and record layouts."""

import random
import struct

import pytest

from app import link_proto as L
from app import protocol as P


def test_crc16_ccitt_false_check_value():
    assert L.crc16(b"123456789") == 0x29B1


@pytest.mark.parametrize("data", [b"", b"\0", b"\0\0", b"\x11\x22\0\x33", bytes(range(1, 255)),
                                  bytes(range(1, 256)), b"\x01" * 254 + b"\0" + b"\x02" * 300])
def test_cobs_roundtrip_edges(data):
    enc = L.cobs_encode(data)
    assert 0 not in enc
    assert L.cobs_decode(enc) == data


def test_cobs_roundtrip_random():
    rnd = random.Random(7)
    for _ in range(500):
        data = bytes(rnd.choice([0, 1, 2, 255, rnd.randrange(256)]) for _ in range(rnd.randrange(600)))
        assert L.cobs_decode(L.cobs_encode(data)) == data


def test_frame_roundtrip_and_damage_detected():
    frame = L.build_frame(L.GET_POINTS, 42, struct.pack("<HB", 0, 50))
    assert frame.endswith(b"\0") and frame.count(b"\0") == 1
    assert L.parse_frame(frame[:-1]) == (1, L.GET_POINTS, 42, b"\0\0\x32")
    bad = bytearray(frame[:-1])
    bad[3] ^= 0x01
    with pytest.raises(L.ProtocolError):
        L.parse_frame(bytes(bad))


def test_point_record_layout_and_crc_changes_with_every_field():
    p = {"id": 7, "line": 1, "module": 2, "channel": 3, "category": "B", "device_type": "PRESSOSTAT",
         "contact": "NC", "delay_s": 30, "enabled": True, "section": 513, "lcd1": "SPRINKLER ÅÄÖ", "lcd2": ""}
    r = P.point_record(p)
    assert len(r) == 52
    assert r[:12] == bytes([7, 0, 1, 2, 3, 1, 3, 1, 30, 1, 1, 2])
    assert r[12:32] == "SPRINKLER ÅÄÖ".encode("latin-1").ljust(20, b"\0")
    base = P.config_crc([p])
    for key, value in [("delay_s", 31), ("enabled", False), ("section", 1), ("lcd2", "X"), ("contact", "NO")]:
        assert P.config_crc([dict(p, **{key: value})]) != base
    # the order of the points does not matter, only their ids
    q = dict(p, id=3, channel=4)
    assert P.config_crc([p, q]) == P.config_crc([q, p])


def test_status_parse():
    payload = struct.pack("<6H6B6IB", 1, 0, 2, 0, 3, 4, 0x0D, 0x04, 2, 2, 0x07 | 0x20, 0x06,
                          60, 1700000000, 99, 5, 0xDEADBEEF, 1234, 1) + bytes([1, 5])
    st = L.parse_status(payload)
    assert st["counts"] == {"A": 1, "B": 0, "C": 2, "DRIFT": 0, "FAULT": 3, "DISABLED": 4}
    assert st["relays_logical"] == {"K1_A": True, "K2_B": False, "K3_FAULT_OK": True, "K4_C": True}
    assert st["relays"] == {"K1_A": False, "K2_B": False, "K3_FAULT_OK": True, "K4_C": False}
    assert st["transmission_blocked"] and st["silenced"] and st["service"] == {"active": True, "remaining_s": 60}
    assert st["leds"]["yellow_b"] == "flash" and st["leds"]["red_a"] and st["leds"]["yellow_service"]
    assert st["sounders"] == "silenced" and st["hmi_lost"]
    assert st["config_crc"] == 0xDEADBEEF and st["last_event_seq"] == 1234
    assert st["lost_modules"] == [[1, 5]]
