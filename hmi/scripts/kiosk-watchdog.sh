#!/bin/bash
# Kiosk watchdog: restarts the kiosk browser if the panel page stops talking to the server
# (frozen page or hung browser). Runs as a systemd service (sprinkler-kiosk-watchdog).
#
# The page polls /api/status several times per second; /api/health reports how many
# seconds ago that last happened (kiosk_age_s). Above LIMIT seconds the browser is killed;
# kiosk.sh then starts it again.
#
# If the server itself is down nothing is done here: systemd restarts the server.

HEALTH="http://127.0.0.1:8000/api/health"
LIMIT=90
CHECK_EVERY=15

while true; do
  sleep "$CHECK_EVERY"
  age="$(curl -s -m 5 "$HEALTH" | python3 -c 'import sys, json; print(json.load(sys.stdin)["kiosk_age_s"])' 2>/dev/null)"
  [ -z "$age" ] && continue
  if [ "$age" -gt "$LIMIT" ]; then
    echo "kiosk-watchdog: no contact from the kiosk page for ${age}s - restarting the browser"
    pkill -f -- "--kiosk" || true
  fi
done
