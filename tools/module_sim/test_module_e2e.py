"""
The real module code (firmware/module/mod_core.c, built for the PC as host_module) on RS485
line 1 instead of the Python simulator, with the real controller (host_app) and the HMI's
SerialController: the same tests the simulator passed, now against the module firmware.

    HOST_APP=/tmp/host_app HOST_MODULE=/tmp/host_module python -m pytest tools/module_sim/test_module_e2e.py
"""
import os
import pty
import subprocess
import time
import tty

import pytest

import test_app_e2e as E

HOST_MODULE = os.environ.get("HOST_MODULE")
pytestmark = pytest.mark.skipif(not (E.HOST_APP and HOST_MODULE),
                                reason="needs HOST_APP and HOST_MODULE binaries")

OHM_CLOSED, OHM_OPEN, OHM_BREAK, OHM_SHORT = 1000, 2000, 0xFFFF, 50


class ModuleBench(E.Bench):
    def __init__(self):
        self.rs_master, rs_slave = pty.openpty()
        tty.setraw(self.rs_master)
        tty.setraw(rs_slave)
        self.rs_slave = rs_slave
        self.mod = subprocess.Popen([HOST_MODULE, "--fd", str(rs_slave), "2"], pass_fds=[rs_slave],
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        assert self.mod.stdout.readline().strip() == "ready"
        self.pi_master, self.pi_slave = pty.openpty()
        tty.setraw(self.pi_master)
        tty.setraw(self.pi_slave)
        self.start_app()
        self.logs = []
        self.link = E.SerialLink(lambda: E.FdPort(self.pi_master), log=self.logs.append)
        self.ctrl = E.SerialController(self.link, start_seq=0)

    def ohm(self, ch, value):
        self.mod.stdin.write(f"ohm {ch} {value}\n")
        self.mod.stdin.flush()

    def supply(self, mv):
        self.mod.stdin.write(f"supply {mv}\n")
        self.mod.stdin.flush()

    def close(self):
        self.proc.kill()
        self.proc.wait(timeout=5)
        self.mod.kill()
        self.mod.wait(timeout=5)
        for fd in (self.rs_master, self.rs_slave, self.pi_master, self.pi_slave):
            os.close(fd)


@pytest.fixture
def bench(tmp_path, monkeypatch):
    monkeypatch.setenv("HOST_FLASH", str(tmp_path / "flash.bin"))
    b = ModuleBench()
    b.wait_until(lambda: b.ctrl.get_status(), 5)
    assert b.ctrl.upload_config(E.POINTS)["ok"]
    b.wait_until(lambda: (b.ctrl.get_status(), any("ONLINE" in line for line in b.logs))[1], 5)
    yield b
    b.close()


def test_module_comes_online_with_its_identity(bench):
    mods = bench.wait_until(lambda: [m for m in bench.ctrl.get_modules()
                                     if m["state"] == "RUN" and m["supply_mv"]])    # details: every 10th poll
    assert mods[0]["address"] == 2 and mods[0]["firmware"] == "1.0" and mods[0]["supply_mv"] == 24000
    st = bench.ctrl.get_status()
    assert st["counts"]["FAULT"] == 0 and st["relays"]["K3_FAULT_OK"] and st["lost_modules"] == []
    assert bench.point(1)["state"] == "NORMAL" and bench.point(2)["state"] == "NORMAL"


def test_valve_and_pressure_switch_through_the_module(bench):
    c = bench.ctrl
    bench.ohm(1, OHM_OPEN)                       # serviceventil (NC) opens: B alarm after debounce
    bench.wait_until(lambda: c.get_status()["counts"]["B"] == 1)
    bench.ohm(1, OHM_CLOSED)
    bench.wait_until(lambda: c.get_status()["counts"]["B"] == 0)
    bench.ohm(2, OHM_CLOSED)                     # pressostat (NO) closes: fire, K1
    bench.wait_until(lambda: c.get_status()["relays"]["K1_A"])
    assert bench.point(2)["state"] == "ALARM"


def test_line_faults_through_the_module(bench):
    c = bench.ctrl
    bench.ohm(1, OHM_BREAK)
    bench.wait_until(lambda: bench.point(1)["fault"] == "OPEN")
    st = c.get_status()
    assert st["counts"]["FAULT"] == 1 and not st["relays"]["K3_FAULT_OK"]
    bench.ohm(1, OHM_SHORT)
    bench.wait_until(lambda: bench.point(1)["fault"] == "SHORT")
    bench.ohm(1, OHM_CLOSED)
    bench.wait_until(lambda: bench.point(1)["fault"] is None and c.get_status()["relays"]["K3_FAULT_OK"])


def test_short_blip_shorter_than_debounce_is_ignored(bench):
    bench.ohm(1, OHM_OPEN)
    time.sleep(0.03)                             # 30 ms < 100 ms debounce
    bench.ohm(1, OHM_CLOSED)
    time.sleep(0.6)
    assert bench.ctrl.get_status()["counts"]["B"] == 0


def test_low_supply_is_reported(bench):
    bench.supply(12000)
    bench.wait_until(lambda: bench.ctrl.get_status()["counts"]["FAULT"] == 1)
    bench.supply(24000)
    bench.wait_until(lambda: bench.ctrl.get_status()["counts"]["FAULT"] == 0)


def test_module_lost_and_back(bench):
    c = bench.ctrl
    bench.mod.send_signal(19)                    # SIGSTOP: the module stops answering
    try:
        bench.wait_until(lambda: c.get_status()["lost_modules"] == [[1, 2]], timeout=6)
    finally:
        bench.mod.send_signal(18)                # SIGCONT
    bench.wait_until(lambda: c.get_status()["lost_modules"] == [] and
                     c.get_status()["relays"]["K3_FAULT_OK"], timeout=6)
