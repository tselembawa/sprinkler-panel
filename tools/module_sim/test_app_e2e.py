"""
End-to-end test of the whole controller application (firmware/controller/app.c with the
alarm engine), built for the PC with a small HAL stand-in, against the module simulator:
exactly what runs on the NUCLEO, minus the hardware.

    HOST_APP=/path/to/host_app python -m pytest tools/module_sim/test_app_e2e.py
"""
import os
import pty
import queue
import re
import subprocess
import sys
import threading
import time
import tty

import pytest

sys.path.insert(0, os.path.dirname(__file__))
import module_sim as ms  # noqa: E402

HOST_APP = os.environ.get("HOST_APP")
pytestmark = pytest.mark.skipif(not HOST_APP or ms.serial is None,
                                reason="needs HOST_APP binary and pyserial")


class App:
    def __init__(self):
        self.master, slave = pty.openpty()
        tty.setraw(self.master)
        tty.setraw(slave)
        self.module = ms.Module(2, log=lambda *_: None)
        self.stop = threading.Event()
        self.sim = threading.Thread(target=ms.run, args=(os.ttyname(slave), [2]),
                                    kwargs=dict(keys=False, modules=[self.module], stop=self.stop),
                                    daemon=True)
        self.sim.start()
        time.sleep(0.2)
        self.proc = subprocess.Popen([HOST_APP, str(self.master)], pass_fds=[self.master],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        self.lines = queue.Queue()
        self.log = []
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
        buf = b""
        while True:
            chunk = self.proc.stdout.read(1)
            if not chunk:
                return
            buf += chunk
            if chunk == b"\n":
                line = buf.decode("ascii", "replace").rstrip()
                self.log.append(line)
                self.lines.put(line)
                buf = b""

    def expect(self, pattern, timeout=4.0):
        end = time.time() + timeout
        rx = re.compile(pattern)
        while time.time() < end:
            try:
                line = self.lines.get(timeout=max(0.01, end - time.time()))
            except queue.Empty:
                break
            if rx.search(line):
                return line
        raise AssertionError(f"not seen: {pattern!r}\n--- last output ---\n" + "\n".join(self.log[-25:]))

    def key(self, c):
        self.proc.stdin.write(c.encode())
        self.proc.stdin.flush()

    def wait_until(self, cond, timeout=4.0):
        end = time.time() + timeout
        while time.time() < end:
            if cond():
                return True
            time.sleep(0.05)
        raise AssertionError("condition not reached\n" + "\n".join(self.log[-25:]))

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=5)
        self.stop.set()
        self.sim.join(timeout=2)
        os.close(self.master)


@pytest.fixture
def app():
    a = App()
    a.expect(r"ONLINE")
    yield a
    a.close()


def sounders_on(m):
    return ms.nibble(m.out_cmd, 0) == ms.CMD_ON


def test_fire_alarm_relays_sounders_silence_and_reset(app):
    m = app.module
    assert any("K3 Fel=OK" in line for line in app.log)         # at start everything is OK
    m.set_contact(1, ms.IN_CLOSED)                              # pressostat (NO) closes: water
    app.expect(r"P2 PRESSOSTAT\s+ALARM_ON\s+NORMAL -> ALARM\s+\(A\)")
    app.expect(r"K1 A=TILL")
    app.wait_until(lambda: sounders_on(m))                      # OUT1 of the module = sounders
    app.key("s")
    app.expect(r"Tysta larmdon: NEKAD")                         # needs the key
    app.key("t")
    app.expect(r"Nyckel: TEKNIKER")
    app.key("s")
    app.expect(r"Tysta larmdon: OK")
    app.wait_until(lambda: not sounders_on(m))
    m.set_contact(1, ms.IN_OPEN)                                # water stops: latched
    app.expect(r"P2 PRESSOSTAT\s+ALARM_CLEARED\s+ALARM -> LATCHED")
    app.key("a")
    app.expect(r"Aterstall: 1 aterstallda, 0 kvar")
    app.expect(r"K1 A=fran")


def test_b_alarm_acknowledged_becomes_c(app):
    m = app.module
    m.set_contact(0, ms.IN_OPEN)                                # serviceventil (NC) opens
    app.expect(r"P1 SERVICEVENTIL\s+ALARM_ON")
    app.expect(r"K2 B=TILL")
    app.expect(r"B/C=BLINKAR.*Summer=PA")
    app.key("k")
    app.expect(r"Kvittera: 1 kvitterade")
    app.expect(r"K2 B=fran.*K4 C=TILL")
    app.expect(r"B/C=fast.*Summer=av")
    m.set_contact(0, ms.IN_CLOSED)
    app.expect(r"P1 SERVICEVENTIL\s+ALARM_CLEARED\s+ACKED -> NORMAL")


def test_line_fault_module_lost_and_service_mode(app):
    m = app.module
    m.set_contact(0, ms.IN_LINE_OPEN)
    app.expect(r"P1 SERVICEVENTIL\s+FEL: Avbrott")
    app.expect(r"K3 Fel=FEL")
    m.set_contact(0, ms.IN_CLOSED)
    app.expect(r"linjen OK igen")
    app.expect(r"K3 Fel=OK")
    m.silent = True
    app.expect(r"Modul L1-M02 SAKNAS", timeout=6)
    app.expect(r"K3 Fel=FEL")
    app.key("t")
    app.key("v")
    app.expect(r"SERVICELAGE 30 min")
    app.expect(r"K3 Fel=OK\(dragen\).*SERVICE: blockerat")       # nothing transmitted
    m.silent = False
    app.expect(r"Modul L1-M02 ONLINE", timeout=6)
