"""
End-to-end test of the whole controller (firmware/controller/app.c + alarm engine + pi_link),
built for the PC with a small HAL stand-in, between the module simulator (RS485 side) and
the HMI's SerialController (Raspberry Pi side): exactly what runs on the NUCLEO, minus the
hardware.

    HOST_APP=/path/to/host_app python -m pytest tools/module_sim/test_app_e2e.py
"""
import os
import pty
import select
import struct
import subprocess
import sys
import threading
import time
import tty

import pytest

HERE = os.path.dirname(__file__)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "hmi"))
import module_sim as ms  # noqa: E402
from app import link_proto as L  # noqa: E402
from app import protocol as P  # noqa: E402
from app.serial_controller import SerialController, SerialLink  # noqa: E402
from app.sim_controller import CommandError, ControllerUnavailable  # noqa: E402

HOST_APP = os.environ.get("HOST_APP")
pytestmark = pytest.mark.skipif(not HOST_APP or ms.serial is None,
                                reason="needs HOST_APP binary and pyserial")


class FdPort:
    """The Pi's end of the link: a pseudo terminal instead of /dev/ttyACM0."""

    def __init__(self, fd):
        self.fd = fd

    def read(self, n):
        r, _, _ = select.select([self.fd], [], [], 0.02)
        return os.read(self.fd, n) if r else b""

    def write(self, data):
        os.write(self.fd, data)

    def close(self):
        pass


POINTS = [
    {"id": 1, "line": 1, "module": 2, "channel": 1, "lcd1": "SERVICEVENTIL", "lcd2": "SEKTION 1",
     "category": "B", "device_type": "SERVICEVENTIL", "contact": "NC", "delay_s": 0, "section": 1,
     "enabled": True, "name": "Serviceventil sektion 1"},
    {"id": 2, "line": 1, "module": 2, "channel": 2, "lcd1": "PRESSOSTAT", "lcd2": "SEKTION 1 ÅÄÖ",
     "category": "A", "device_type": "PRESSOSTAT", "contact": "NO", "delay_s": 0, "section": 1,
     "enabled": True, "name": "Pressostat sektion 1"},
]


class Bench:
    def __init__(self, start_seq=0):
        # RS485 line 1 <-> module simulator
        self.rs_master, rs_slave = pty.openpty()
        tty.setraw(self.rs_master)
        tty.setraw(rs_slave)
        self.module = ms.Module(2, log=lambda *_: None)
        self.stop = threading.Event()
        self.sim = threading.Thread(target=ms.run, args=(os.ttyname(rs_slave), [2]),
                                    kwargs=dict(keys=False, modules=[self.module], stop=self.stop),
                                    daemon=True)
        self.sim.start()
        time.sleep(0.2)
        # Pi link: host_app stdin/stdout <-> pty <-> SerialController
        self.pi_master, self.pi_slave = pty.openpty()
        tty.setraw(self.pi_master)
        tty.setraw(self.pi_slave)
        self.start_app()
        self.logs = []
        self.link = SerialLink(lambda: FdPort(self.pi_master), log=self.logs.append)
        self.ctrl = SerialController(self.link, start_seq=start_seq)

    def start_app(self):
        self.proc = subprocess.Popen([HOST_APP, str(self.rs_master)], pass_fds=[self.rs_master],
                                     stdin=self.pi_slave, stdout=self.pi_slave)

    def restart_app(self):
        self.proc.kill()
        self.proc.wait(timeout=5)
        time.sleep(0.2)
        self.start_app()

    def wait_until(self, cond, timeout=4.0):
        end = time.time() + timeout
        last = None
        while time.time() < end:
            try:
                last = cond()
                if last:
                    return last
            except (AssertionError, ControllerUnavailable):
                pass
            time.sleep(0.05)
        raise AssertionError(f"condition not reached (last: {last!r})\n" + "\n".join(self.logs[-25:]))

    def point(self, pid):
        return next(p for p in self.ctrl.get_points() if p["id"] == pid)

    def close(self):
        self.proc.kill()
        self.proc.wait(timeout=5)
        self.stop.set()
        self.sim.join(timeout=2)
        for fd in (self.rs_master, self.pi_master, self.pi_slave):
            os.close(fd)


@pytest.fixture
def bench():
    b = Bench(start_seq=100)
    b.wait_until(lambda: (b.ctrl.get_status(), any("ONLINE" in line for line in b.logs))[1], 5)
    yield b
    b.close()


def sounders_on(m):
    return ms.nibble(m.out_cmd, 0) == ms.CMD_ON


def test_status_time_sync_and_log(bench):
    st = bench.ctrl.get_status()
    assert st["firmware"] == "0.3" and st["protocol"] == 1
    assert st["relays"]["K3_FAULT_OK"] and not st["relays"]["K1_A"]
    assert abs(st["time"] - time.time()) < 5          # the Pi set the controller clock
    assert st["leds"]["green_power"] and not st["hmi_lost"]
    assert any(line.startswith("Sprinkler controller fw 0.3") for line in bench.logs) or \
        any("ONLINE" in line for line in bench.logs)


def test_upload_config_fire_alarm_silence_and_reset(bench):
    c, m = bench.ctrl, bench.module
    res = c.upload_config(POINTS)
    assert res["ok"], res
    assert res["crc"] == P.config_crc(POINTS)
    assert c.config_info() == {"version": res["version"], "crc": P.config_crc(POINTS), "points": 2}
    bench.wait_until(lambda: any("Konfiguration v" in line for line in bench.logs))
    bench.wait_until(lambda: c.get_status()["lost_modules"] == [])

    m.set_contact(1, ms.IN_CLOSED)                     # pressostat (NO) closes: water
    bench.wait_until(lambda: bench.point(2)["state"] == "ALARM")
    st = c.get_status()
    assert st["relays"]["K1_A"] and st["counts"]["A"] == 1 and st["buzzer"]
    bench.wait_until(lambda: sounders_on(m))

    with pytest.raises(CommandError) as e:
        c.cmd_silence(True)
    assert e.value.code == "ACCESS_DENIED"            # needs the key
    c.dev_set_key(True)
    assert c.get_status()["key_tech"]
    assert c.cmd_silence(True) == {"silenced": True}
    bench.wait_until(lambda: not sounders_on(m))

    m.set_contact(1, ms.IN_OPEN)                       # water stops: latched
    bench.wait_until(lambda: bench.point(2)["state"] == "LATCHED")
    assert c.cmd_reset() == {"reset": [2], "cause_remains": []}
    assert bench.point(2)["state"] == "NORMAL"
    assert not c.get_status()["relays"]["K1_A"]


def test_b_alarm_ack_becomes_c_and_events(bench):
    c, m = bench.ctrl, bench.module
    assert c.upload_config(POINTS)["ok"]
    bench.wait_until(lambda: c.get_status()["lost_modules"] == [])
    m.set_contact(0, ms.IN_OPEN)                       # serviceventil (NC) opens
    bench.wait_until(lambda: bench.point(1)["state"] == "ALARM")
    assert c.get_status()["leds"]["yellow_b"] == "flash"
    assert c.cmd_ack() == {"acknowledged": 1}
    p1 = bench.point(1)
    assert p1["state"] == "ACKED" and p1["display_category"] == "C"
    st = c.get_status()
    assert st["relays"]["K4_C"] and not st["relays"]["K2_B"] and st["leds"]["yellow_b"] == "steady"

    ev = c.get_events(0, 500)
    assert ev[0]["seq"] == 101 and ev[0]["type"] == "STARTUP"     # HMI numbering continues at 100
    assert [e["seq"] for e in ev] == list(range(101, 101 + len(ev)))
    on = [e for e in ev if e["type"] == "ALARM_ON" and e["source_id"] == 1]
    assert on and on[-1]["old"] == "NORMAL" and on[-1]["new"] == "ALARM" and on[-1]["source_kind"] == "POINT"
    assert any(e["type"] == "CONFIG_ACTIVATED" and e["new"] == "1" for e in ev)
    assert c.get_status()["last_event_seq"] == ev[-1]["seq"]


def test_line_fault_module_lost_disable_and_service(bench):
    c, m = bench.ctrl, bench.module
    assert c.upload_config(POINTS)["ok"]
    bench.wait_until(lambda: c.get_status()["lost_modules"] == [])
    m.set_contact(0, ms.IN_LINE_OPEN)
    bench.wait_until(lambda: bench.point(1)["fault"] == "OPEN" and bench.point(1)["condition"] == "OPEN")
    assert not c.get_status()["relays"]["K3_FAULT_OK"]
    m.set_contact(0, ms.IN_CLOSED)
    bench.wait_until(lambda: bench.point(1)["fault"] is None)

    m.silent = True
    bench.wait_until(lambda: c.get_status()["lost_modules"] == [[1, 2]], 6)
    assert bench.point(2)["fault"] == "MODULE_LOST"
    m.silent = False
    bench.wait_until(lambda: c.get_status()["lost_modules"] == [], 6)

    c.dev_set_key(True)
    assert c.cmd_disable(2, True) == {"point_id": 2, "enabled": False}
    assert bench.point(2)["enabled"] is False and c.get_status()["counts"]["DISABLED"] == 1
    with pytest.raises(CommandError) as e:
        c.cmd_disable(77, True)
    assert e.value.code == "UNKNOWN_POINT"
    assert c.cmd_service(30) == {"service": True}
    st = c.get_status()
    assert st["transmission_blocked"] and st["service"]["active"] and 1790 <= st["service"]["remaining_s"] <= 1800
    assert c.cmd_service(0) == {"service": False}
    with pytest.raises(CommandError) as e:
        c.cmd_service(481)
    assert e.value.code == "BAD_DURATION"


def test_config_rejected_and_crc_mismatch(bench):
    c = bench.ctrl
    bad = [dict(POINTS[0]), dict(POINTS[1], channel=1)]           # same address twice
    res = c.upload_config(bad)
    assert not res["ok"] and res["errors"][0]["code"] == "DUPLICATE_ADDRESS"
    # bypass the Pi's own check: the controller must find the same thing
    rec = b"".join(P.point_record(p) for p in bad)
    c._req(L.CFG_BEGIN, struct.pack("<H", 2))
    c._req(L.CFG_PUT, rec)
    errors = L.parse_cfg_errors(c._req(L.CFG_END, struct.pack("<I", P.config_crc(bad))))
    assert errors == [{"point_id": 2, "field": "channel", "code": "DUPLICATE_ADDRESS"}]
    with pytest.raises(CommandError) as e:
        c._req(L.CFG_ACTIVATE, b"\0")                              # not after a failed check
    assert e.value.code == "CFG_SEQUENCE"
    # wrong CRC (a damaged record): rejected, controller tells its own CRC
    c._req(L.CFG_BEGIN, struct.pack("<H", 1))
    c._req(L.CFG_PUT, P.point_record(POINTS[0]))
    with pytest.raises(CommandError) as e:
        c._req(L.CFG_END, struct.pack("<I", 0x12345678))
    assert e.value.code == "CFG_CRC_MISMATCH"
    assert int.from_bytes(bytes.fromhex(e.value.detail), "little") == P.config_crc([POINTS[0]])
    assert c.config_info()["points"] == 2                           # the old one still runs


def test_repeated_command_is_not_run_twice(bench):
    port = FdPort(bench.pi_master)
    c = bench.ctrl
    c.dev_set_key(True)
    before = c.get_status()["last_event_seq"]
    frame = L.build_frame(L.CMD_SILENCE, 77, bytes([0, 1]))
    answers = []
    with bench.link.lock:
        for _ in range(2):                              # the Pi "did not hear" the first answer
            port.write(frame)
            buf = b""
            end = time.time() + 1
            while time.time() < end and b"\0" not in buf:
                buf += port.read(256)
            for raw in buf.split(b"\0"):
                if raw:
                    ver, t, seq, payload = L.parse_frame(raw)
                    if t != L.LOG:
                        answers.append((t, seq, payload))
    assert answers == [(L.CMD_SILENCE, 77, b"\x01")] * 2       # the same answer twice ...
    ev = [e for e in c.get_events(before, 50) if e["type"] == "SILENCE"]
    assert len(ev) == 1                                          # ... but done only once


def test_hmi_lost_and_back(bench):
    c = bench.ctrl
    assert not c.get_status()["hmi_lost"]
    time.sleep(6)                                       # the Pi says nothing for > 5 s
    ev = c.get_events(0, 500)
    types = [e["type"] for e in ev]
    assert "HMI_LOST" in types and types.index("HMI_LOST") < len(types) - 1 and types[-1] == "HMI_BACK"


def test_controller_restart_keeps_event_numbering(bench):
    c = bench.ctrl
    first = c.get_events(0, 500)
    last = first[-1]["seq"]
    bench.restart_app()
    time.sleep(0.5)
    st = c.get_status()                                 # uptime went back: restart noticed
    ev = c.get_events(last, 500)
    assert ev and ev[0]["seq"] == last + 1 and ev[0]["type"] == "STARTUP"
    assert st["last_event_seq"] >= last + 1
