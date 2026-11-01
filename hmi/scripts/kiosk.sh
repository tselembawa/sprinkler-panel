#!/bin/bash
# Starts the panel page full screen in Chromium kiosk mode on the Raspberry Pi.
# Started by the desktop autostart (set up by install-pi.sh).
#  * waits until the HMI server answers
#  * restarts Chromium automatically if it exits or is killed by the kiosk watchdog

URL="http://localhost:8000/"
HEALTH="http://127.0.0.1:8000/api/health"
BROWSER="$(command -v chromium || command -v chromium-browser)"

# Only one kiosk at a time (the script may be started by more than one autostart method).
exec 9>"/tmp/sprinkler-kiosk-$(id -u).lock"
flock -n 9 || exit 0

if [ -z "$BROWSER" ]; then
  echo "kiosk: Chromium not found" >&2
  exit 1
fi

# Wait for the server (it may still be starting after boot).
until curl -s -o /dev/null -m 2 "$HEALTH"; do
  sleep 2
done

while true; do
  "$BROWSER" \
    --kiosk "$URL" \
    --noerrdialogs \
    --disable-infobars \
    --no-first-run \
    --disable-session-crashed-bubble \
    --disable-features=Translate \
    --check-for-update-interval=31536000 \
    --password-store=basic \
    --autoplay-policy=no-user-gesture-required \
    >/dev/null 2>&1
  sleep 3
done
