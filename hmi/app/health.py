"""Health monitoring and the systemd watchdog.

Level 1 of the HMI watchdog chain:
  * The server tells systemd "I am alive" (WATCHDOG=1) only while it is really healthy:
    the controller answers, the database answers and the event sync loop is running.
    If the process hangs, systemd stops getting pings and restarts the service.
  * The kiosk browser on the Pi polls /api/status several times a second. If that stops
    (frozen page or browser), /api/health reports a growing kiosk age and the kiosk
    watchdog script restarts the browser.

Level 2 (the Raspberry Pi hardware watchdog) is configured by scripts/install-pi.sh:
if the whole Linux system freezes, the chip itself reboots the Pi.

The alarm handling never depends on any of this: it lives in the Main Controller.
"""

from __future__ import annotations

import os
import socket
import threading
import time


def sd_notify(message: str) -> bool:
    """Send a notification to systemd (Type=notify services). No-op outside systemd."""
    path = os.environ.get("NOTIFY_SOCKET")
    if not path:
        return False
    if path.startswith("@"):  # abstract namespace socket
        path = "\0" + path[1:]
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as s:
            s.connect(path)
            s.sendall(message.encode())
        return True
    except OSError:
        return False


class HealthMonitor:
    SYNC_STALE_S = 10      # the event sync loop must have run within this time
    KIOSK_STALE_S = 90     # reported to the kiosk watchdog; it decides when to restart

    def __init__(self, controller, db, clock=time.monotonic):
        self._controller = controller
        self._db = db
        self._clock = clock
        self._started = clock()
        self._last_sync = clock()
        self._last_kiosk: float | None = None
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.last_report: dict = {}

    # --- called from the rest of the app
    def sync_ran(self):
        self._last_sync = self._clock()

    def kiosk_seen(self):
        self._last_kiosk = self._clock()

    # --- checks
    def check(self) -> dict:
        now = self._clock()
        checks = {}
        try:
            status = self._controller.get_status()
            checks["controller"] = isinstance(status, dict) and "relays" in status
        except Exception:  # noqa: BLE001 - any failure means unhealthy
            checks["controller"] = False
        try:
            self._db.last_event_seq()
            checks["database"] = True
        except Exception:  # noqa: BLE001
            checks["database"] = False
        checks["event_sync"] = now - self._last_sync < self.SYNC_STALE_S
        since = self._last_kiosk if self._last_kiosk is not None else self._started
        report = {
            "ok": all(checks.values()),
            "checks": checks,
            "uptime_s": int(now - self._started),
            "kiosk_age_s": int(now - since),
            "kiosk_seen": self._last_kiosk is not None,
        }
        self.last_report = report
        return report

    # --- systemd watchdog loop
    def start(self):
        sd_notify("READY=1")
        usec = int(os.environ.get("WATCHDOG_USEC", "0") or 0)
        if usec <= 0:
            return  # not running under a systemd watchdog (e.g. development)
        interval = max(1.0, usec / 1e6 / 3)
        self._thread = threading.Thread(target=self._loop, args=(interval,), daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        sd_notify("STOPPING=1")
        if self._thread:
            self._thread.join(timeout=2)

    def _loop(self, interval: float):
        while not self._stop.wait(interval):
            report = self.check()
            if report["ok"]:
                sd_notify("WATCHDOG=1")
            else:
                bad = ", ".join(k for k, v in report["checks"].items() if not v)
                sd_notify(f"STATUS=Unhealthy: {bad}")
                print(f"HEALTH: unhealthy ({bad}) - not feeding the watchdog", flush=True)
