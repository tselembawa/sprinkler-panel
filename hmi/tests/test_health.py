"""Tests for the health monitor and the systemd notification used by the watchdog."""

import os
import socket
import tempfile

from app.health import HealthMonitor, sd_notify


class FakeClock:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


class FakeController:
    def __init__(self):
        self.broken = False

    def get_status(self):
        if self.broken:
            raise RuntimeError("no answer")
        return {"relays": {}}


class FakeDb:
    def __init__(self):
        self.broken = False

    def last_event_seq(self):
        if self.broken:
            raise RuntimeError("db locked")
        return 0


def make():
    clock, ctl, db = FakeClock(), FakeController(), FakeDb()
    return HealthMonitor(ctl, db, clock=clock), clock, ctl, db


def test_healthy_when_everything_answers():
    h, *_ = make()
    assert h.check()["ok"]


def test_unhealthy_when_controller_or_db_fails():
    h, _, ctl, db = make()
    ctl.broken = True
    r = h.check()
    assert not r["ok"] and not r["checks"]["controller"]
    ctl.broken = False
    db.broken = True
    assert not h.check()["checks"]["database"]


def test_unhealthy_when_event_sync_stops():
    h, clock, *_ = make()
    clock.t += 11
    assert not h.check()["checks"]["event_sync"]
    h.sync_ran()
    assert h.check()["ok"]


def test_kiosk_age_counts_from_start_until_seen():
    h, clock, *_ = make()
    clock.t += 50
    r = h.check()
    assert r["kiosk_age_s"] == 50 and not r["kiosk_seen"]
    h.kiosk_seen()
    h.sync_ran()
    clock.t += 5
    r = h.check()
    assert r["kiosk_age_s"] == 5 and r["kiosk_seen"]


def test_sd_notify_sends_to_systemd_socket(monkeypatch):
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "notify.sock")
        srv = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        srv.bind(path)
        srv.settimeout(1)
        monkeypatch.setenv("NOTIFY_SOCKET", path)
        assert sd_notify("WATCHDOG=1")
        assert srv.recv(64) == b"WATCHDOG=1"
        srv.close()


def test_sd_notify_is_noop_outside_systemd(monkeypatch):
    monkeypatch.delenv("NOTIFY_SOCKET", raising=False)
    assert not sd_notify("WATCHDOG=1")
