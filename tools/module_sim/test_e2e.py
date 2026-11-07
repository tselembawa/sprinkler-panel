"""
End-to-end test on Linux: the controller's module protocol (C, built for the PC as
firmware/tests/host_ctrl) talks Modbus over a pseudo terminal to this module simulator
(running its real serial loop with pyserial). Run:

    gcc ... -o host_ctrl  (see firmware/tests/README.md)
    HOST_CTRL=/path/to/host_ctrl python -m pytest tools/module_sim/test_e2e.py
"""
import os
import pty
import subprocess
import sys
import threading
import time
import tty

import pytest

sys.path.insert(0, os.path.dirname(__file__))
import module_sim as ms  # noqa: E402

HOST_CTRL = os.environ.get("HOST_CTRL")
pytestmark = pytest.mark.skipif(not HOST_CTRL or ms.serial is None,
                                reason="needs HOST_CTRL binary and pyserial")


class Rig:
    def __init__(self, specs, sim_addrs):
        self.master, slave = pty.openpty()
        tty.setraw(self.master)
        tty.setraw(slave)
        self.port = os.ttyname(slave)
        self.logs = []
        self.modules = [ms.Module(a, log=self.logs.append) for a in sim_addrs]
        self.stop = threading.Event()
        self.thread = threading.Thread(
            target=ms.run, args=(self.port, sim_addrs),
            kwargs=dict(keys=False, modules=self.modules, stop=self.stop), daemon=True)
        self.thread.start()
        time.sleep(0.2)
        self.proc = subprocess.Popen([HOST_CTRL, str(self.master)] + specs, pass_fds=[self.master],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.slave = slave

    def cmd(self, line):
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()
        events = []
        while True:
            out = self.proc.stdout.readline().strip()
            if out == "done" or out == "":
                return events
            events.append(out)

    def poll(self):
        return self.cmd("poll")

    def close(self):
        self.cmd("quit")
        self.proc.wait(timeout=5)
        self.stop.set()
        self.thread.join(timeout=2)
        os.close(self.master)


@pytest.fixture
def rig():
    r = Rig(["2:1"], [2])
    yield r
    r.close()


def names(events):
    return [e.split()[2] for e in events]


def test_start_configure_and_report_inputs(rig):
    m = rig.modules[0]
    assert m.flags & ms.ST_RESTARTED
    assert rig.poll() == ["EV 2 ONLINE 0 0 0"]
    assert not m.flags & ms.ST_RESTARTED                      # CONFIG_DONE received
    assert [c & 0xFF for c in m.cfg[4:8]] == [1, 1, 0, 0]     # IN3/IN4 not used
    assert m.cfg[0] == 10 and m.cfg[1] == 0x2222
    ev = rig.poll()
    # first data: all four inputs reported, old value 65535 = unknown
    assert ev == ["EV 2 INPUT 0 65535 0", "EV 2 INPUT 1 65535 1",
                  "EV 2 INPUT 2 65535 4", "EV 2 INPUT 3 65535 4"]
    assert rig.poll() == []


def test_input_change_transient_and_line_fault(rig):
    rig.poll(), rig.poll()
    m = rig.modules[0]
    m.set_contact(1, ms.IN_CLOSED)                             # pressostat closes: water
    assert rig.poll() == ["EV 2 INPUT 1 1 0"]
    m.set_contact(0, ms.IN_OPEN)                               # quick pulse on IN1
    m.set_contact(0, ms.IN_CLOSED)
    assert rig.poll() == ["EV 2 TRANSIENT 0 0 2"]
    m.set_contact(0, ms.IN_LINE_OPEN)                          # wire cut
    assert rig.poll() == ["EV 2 INPUT 0 0 2"]


def test_outputs_and_bad_command(rig):
    rig.poll(), rig.poll()
    m = rig.modules[0]
    rig.cmd("out 0 1")
    rig.poll()
    assert m.out_cmd == 0x0001
    rig.cmd("out 0 7")                                         # invalid: module answers exception 03
    ev = rig.poll()
    assert names(ev) == ["COMM_ERROR"] and ev[0].split()[4] == "4"   # MB_ERR_EXCEPTION
    assert m.out_cmd == 0x0001
    rig.cmd("out 0 0")
    rig.poll()
    assert m.out_cmd == 0


def test_module_lost_and_back(rig):
    rig.poll(), rig.poll()
    m = rig.modules[0]
    m.silent = True
    ev = rig.poll() + rig.poll() + rig.poll()
    assert names(ev) == ["COMM_ERROR", "COMM_ERROR", "COMM_ERROR", "LOST"]
    assert rig.poll() == []                                    # LOST reported once
    m.silent = False
    assert rig.poll() == ["EV 2 ONLINE 0 0 0"]
    assert names(rig.poll()) == ["INPUT"] * 4


def test_module_restart_is_reconfigured(rig):
    rig.poll(), rig.poll()
    m = rig.modules[0]
    m.power_on()
    assert m.flags & ms.ST_RESTARTED
    assert rig.poll() == ["EV 2 RESTARTED 0 0 0", "EV 2 ONLINE 0 0 0"]
    assert not m.flags & ms.ST_RESTARTED


def test_wrong_line_and_missing_module():
    r = Rig(["2:1", "61:1", "5:1"], [2, 61])                   # 61 belongs to line 2; 5 does not exist
    try:
        ev = r.poll()
        assert "EV 2 ONLINE 0 0 0" in ev
        assert "EV 61 WRONG_LINE 0 1 2" in ev
        ev = r.poll() + r.poll()
        assert "EV 5 LOST 0 1 0" in ev                         # never answered: LOST after 3 tries
        assert sum(e.startswith("EV 61") for e in ev) == 0      # wrong line reported only once
        assert r.modules[1].flags & ms.ST_RESTARTED            # and never configured
    finally:
        r.close()


def test_identify(rig):
    rig.poll()
    m = rig.modules[0]
    rig.cmd("ident 0 1")
    assert m.status() & ms.ST_IDENTIFY
    rig.cmd("ident 0 0")
    assert not m.status() & ms.ST_IDENTIFY
