"""Tests for the alarm logic agreed in the specification.

Run from the hmi folder:  python -m pytest -q
"""

import pytest

from app import protocol as P
from app.sim_controller import CommandError, SimController


class FakeClock:
    def __init__(self):
        self.t = 1000.0

    def __call__(self):
        return self.t

    def advance(self, s):
        self.t += s


def point(pid, category="B", device_type="SERVICEVENTIL", delay=0, line=1, module=1, channel=None):
    return {
        "id": pid, "line": line, "module": module, "channel": channel or pid,
        "name": f"Punkt {pid}", "lcd1": "TEST", "lcd2": f"PUNKT {pid}",
        "category": category, "device_type": device_type, "contact": "NC",
        "delay_s": delay, "section": 1, "enabled": True,
    }


@pytest.fixture
def ctl():
    clock = FakeClock()
    c = SimController(clock=clock, wall=clock)
    c.clock = clock
    return c


def state(c, pid):
    return next(p for p in c.get_points() if p["id"] == pid)


def test_saved_only_when_crc_matches(ctl):
    pts = [point(1)]
    res = ctl.upload_config(pts)
    assert res["ok"]
    assert res["crc"] == P.config_crc(pts)
    assert ctl.config_info()["crc"] == P.config_crc(pts)


def test_invalid_config_is_rejected_and_old_one_kept(ctl):
    ctl.upload_config([point(1)])
    before = ctl.config_info()
    res = ctl.upload_config([point(1, delay=61), point(2, channel=1)])
    assert not res["ok"]
    codes = {e["code"] for e in res["errors"]}
    assert "DELAY_NOT_ALLOWED" in codes
    assert "DUPLICATE_ADDRESS" in codes
    assert ctl.config_info() == before


def test_delay_cancelled_and_expired(ctl):
    ctl.upload_config([point(1, category="A", device_type="PRESSOSTAT", delay=40)])
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    assert state(ctl, 1)["state"] == "PENDING"
    ctl.clock.advance(39)
    ctl.tick()
    assert state(ctl, 1)["state"] == "PENDING"
    ctl.sim_set_input(1, 1, 1, "NORMAL")
    assert state(ctl, 1)["state"] == "NORMAL"
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    ctl.clock.advance(41)
    ctl.tick()
    assert state(ctl, 1)["state"] == "ALARM"
    assert ctl.get_status()["relays"]["K1_A"]


def test_fire_alarm_latches_until_reset_with_key(ctl):
    ctl.upload_config([point(1, category="A", device_type="FLODESVAKT")])
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    ctl.sim_set_input(1, 1, 1, "NORMAL")
    assert state(ctl, 1)["state"] == "LATCHED"
    assert ctl.get_status()["relays"]["K1_A"]
    with pytest.raises(CommandError):
        ctl.cmd_reset()  # key switch not turned
    ctl.sim_set_key(True)
    ctl.cmd_reset()
    assert state(ctl, 1)["state"] == "NORMAL"
    assert not ctl.get_status()["relays"]["K1_A"]


def test_reset_does_not_clear_active_fire_alarm(ctl):
    ctl.upload_config([point(1, category="A", device_type="PRESSOSTAT")])
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    ctl.sim_set_key(True)
    res = ctl.cmd_reset()
    assert res["cause_remains"] == [1]
    assert state(ctl, 1)["state"] == "ALARM"


def test_non_latching_point_resets_itself(ctl):
    ctl.upload_config([point(1, category="B", device_type="SERVICEVENTIL")])
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    assert state(ctl, 1)["state"] == "ALARM"
    ctl.sim_set_input(1, 1, 1, "NORMAL")
    assert state(ctl, 1)["state"] == "NORMAL"


def test_acknowledged_b_alarm_becomes_c_and_rearms_k2(ctl):
    ctl.upload_config([point(1), point(2)])
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    s = ctl.get_status()
    assert s["relays"]["K2_B"] and not s["relays"]["K4_C"]
    assert s["leds"]["yellow_b"] == "flash"

    ctl.cmd_ack()
    s = ctl.get_status()
    assert not s["relays"]["K2_B"] and s["relays"]["K4_C"]
    assert s["leds"]["yellow_b"] == "steady"
    assert state(ctl, 1)["display_category"] == "C"

    # A new B-larm must be transmitted again on K2.
    ctl.sim_set_input(1, 1, 2, "ACTIVE")
    s = ctl.get_status()
    assert s["relays"]["K2_B"] and s["relays"]["K4_C"]

    # The C-larm disappears by itself when the cause goes away.
    ctl.sim_set_input(1, 1, 1, "NORMAL")
    assert state(ctl, 1)["state"] == "NORMAL"


def test_a_alarm_never_becomes_c(ctl):
    ctl.upload_config([point(1, category="A", device_type="FLODESVAKT")])
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    ctl.cmd_ack()
    assert state(ctl, 1)["display_category"] == "A"
    assert not ctl.get_status()["relays"]["K4_C"]


def test_line_fault_drops_k3_and_keeps_alarm(ctl):
    ctl.upload_config([point(1, category="A", device_type="FLODESVAKT")])
    assert ctl.get_status()["relays"]["K3_FAULT_OK"]
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    ctl.sim_set_input(1, 1, 1, "OPEN")
    p = state(ctl, 1)
    assert p["fault"] == "OPEN"
    assert p["state"] == "ALARM"  # an existing alarm is never hidden by a fault
    assert not ctl.get_status()["relays"]["K3_FAULT_OK"]


def test_module_lost_is_a_fault(ctl):
    ctl.upload_config([point(1)])
    ctl.sim_set_module_lost(1, 1, True)
    assert state(ctl, 1)["fault"] == "MODULE_LOST"
    assert not ctl.get_status()["relays"]["K3_FAULT_OK"]
    ctl.sim_set_module_lost(1, 1, False)
    assert ctl.get_status()["relays"]["K3_FAULT_OK"]


def test_buzzer_stops_when_alarm_or_fault_goes_away_by_itself(ctl):
    """The buzzer follows what is there NOW (2026-11-13): what went away by itself is in the
    event log but needs no Kvittera. Mirrors the C test of the same name."""
    ctl.upload_config([point(1), point(2)])
    buzz = lambda: ctl.get_status()["buzzer"]
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    assert buzz()
    ctl.sim_set_input(1, 1, 1, "NORMAL")
    assert not buzz() and ctl.get_status()["counts"]["C"] == 0

    for cond in ("ACTIVE", "OPEN", "SHORT", "NORMAL"):        # the simulator's old key cycle
        ctl.sim_set_input(1, 1, 1, cond)
    st = ctl.get_status()
    assert not st["buzzer"] and st["counts"]["FAULT"] == 0 and st["relays"]["K3_FAULT_OK"]
    types = {e["type"] for e in ctl.get_events(0, 100)}
    assert {"LINE_OPEN", "LINE_SHORT", "LINE_OK"} <= types

    ctl.sim_set_input(1, 1, 1, "OPEN")                         # a fault that stays
    assert buzz()
    assert ctl.cmd_ack()["acknowledged"] > 0
    assert not buzz() and ctl.get_status()["counts"]["FAULT"] == 1
    ctl.sim_set_input(1, 1, 2, "OPEN")                         # a second one comes and goes
    assert buzz()
    ctl.sim_set_input(1, 1, 2, "NORMAL")
    assert not buzz() and ctl.get_status()["counts"]["FAULT"] == 1

    ctl.sim_set_input(1, 1, 1, "NORMAL")
    ctl.sim_set_module_lost(1, 1, True)
    assert buzz()
    ctl.sim_set_module_lost(1, 1, False)
    assert not buzz()
    ctl.sim_set_module_lost(1, 1, True)
    ctl.cmd_ack()
    assert not buzz() and ctl.get_status()["counts"]["FAULT"] > 0


def test_disabled_point_gives_no_alarm(ctl):
    ctl.upload_config([point(1)])
    ctl.sim_set_key(True)
    ctl.cmd_disable(1, True)
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    assert state(ctl, 1)["state"] == "NORMAL"
    assert ctl.get_status()["counts"]["DISABLED"] == 1
    # Disablement is runtime only and must not change the configuration CRC.
    assert ctl.config_info()["crc"] == P.config_crc([point(1)])


def test_events_have_increasing_sequence(ctl):
    ctl.upload_config([point(1)])
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    ctl.cmd_ack()
    events = ctl.get_events(0)
    seqs = [e["seq"] for e in events]
    assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs)
    later = ctl.get_events(seqs[-2])
    assert [e["seq"] for e in later] == [seqs[-1]]


def test_set_time_requires_key_and_is_logged(ctl):
    with pytest.raises(CommandError):
        ctl.set_time(1_800_000_000)
    ctl.sim_set_key(True)
    ctl.set_time(1_800_000_000)
    assert abs(ctl.get_status()["time"] - 1_800_000_000) < 1
    ev = ctl.get_events(0)[-1]
    assert ev["type"] == "TIME_SET" and abs(ev["time"] - 1_800_000_000) < 1
    with pytest.raises(CommandError):
        ctl.set_time(5)  # nonsense time is rejected


def test_silence_sounders_and_resound_on_new_alarm(ctl):
    ctl.upload_config([point(1, category="A", device_type="FLODESVAKT"),
                       point(2, category="A", device_type="PRESSOSTAT")])
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    assert ctl.get_status()["sounders"] == "sounding"
    with pytest.raises(CommandError):
        ctl.cmd_silence(True)  # needs the key
    ctl.sim_set_key(True)
    ctl.cmd_silence(True)
    assert ctl.get_status()["sounders"] == "silenced"
    assert ctl.get_status()["relays"]["K1_A"]  # silencing never stops the alarm transmission
    ctl.sim_set_input(1, 1, 2, "ACTIVE")  # a new fire alarm
    assert ctl.get_status()["sounders"] == "sounding"
    assert any(e["type"] == "RESOUND" for e in ctl.get_events(0))


def test_service_mode_times_out(ctl):
    ctl.upload_config([point(1)])
    ctl.sim_set_key(True)
    ctl.cmd_service(30)
    s = ctl.get_status()
    assert s["service"]["active"] and s["leds"]["yellow_service"]
    assert not s["leds"]["green_power"]  # Drift is off while in service mode
    ctl.clock.advance(29 * 60)
    ctl.tick()
    assert ctl.get_status()["service"]["remaining_s"] == 60
    ctl.clock.advance(61)
    ctl.tick()
    assert not ctl.get_status()["service"]["active"]
    assert ctl.get_status()["leds"]["green_power"]
    assert ctl.get_events(0)[-1]["type"] == "SERVICE_OFF"
    # alarms are detected in service mode, but not transmitted
    ctl.cmd_service(30)
    ctl.sim_set_input(1, 1, 1, "ACTIVE")
    s = ctl.get_status()
    assert s["relays_logical"]["K2_B"] and not s["relays"]["K2_B"]


def test_service_mode_blocks_all_transmission_and_releases_at_end(ctl):
    ctl.upload_config([point(1, category="A", device_type="FLODESVAKT"), point(2)])
    ctl.sim_set_key(True)
    ctl.cmd_service(60)
    ctl.sim_set_input(1, 1, 1, "ACTIVE")   # fire alarm while the technician works
    ctl.sim_set_input(1, 1, 2, "OPEN")     # and a cut cable
    s = ctl.get_status()
    assert s["transmission_blocked"]
    assert s["relays"] == {"K1_A": False, "K2_B": False, "K3_FAULT_OK": True, "K4_C": False}
    assert s["relays_logical"]["K1_A"] and not s["relays_logical"]["K3_FAULT_OK"]
    assert s["leds"]["red_a"] and s["sounders"] == "sounding"   # still shown and heard locally
    ctl.cmd_service(0)                       # service ends: the active alarm goes out at once
    s = ctl.get_status()
    assert s["relays"]["K1_A"] and not s["relays"]["K3_FAULT_OK"] and not s["transmission_blocked"]
