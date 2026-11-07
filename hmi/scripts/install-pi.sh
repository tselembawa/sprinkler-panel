#!/bin/bash
# Installs the sprinkler panel HMI on a Raspberry Pi (Raspberry Pi OS with desktop).
#
# Run once as the normal user (not root), from the repository:
#     cd ~/sprinkler-panel && bash hmi/scripts/install-pi.sh
# then reboot. With the real controller (Nucleo on a USB port of the Pi):
#     SPRINKLER_CONTROLLER=serial bash hmi/scripts/install-pi.sh
# (default: sim, the virtual controller inside the HMI).
#
# What it sets up:
#   1. Python environment and packages
#   2. sprinkler-hmi service: starts at boot, restarts on crash, systemd watchdog (level 1)
#   3. Kiosk: desktop auto-login, Chromium full screen on the panel page, no screen blanking
#   4. sprinkler-kiosk-watchdog service: restarts the browser if the page freezes (level 1)
#   5. Hardware watchdog of the Pi (level 2): if Linux freezes, the chip reboots the Pi

set -euo pipefail

if [ "$(id -u)" -eq 0 ]; then
  echo "Run this script as your normal user (it uses sudo where needed)." >&2
  exit 1
fi

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
USER_NAME="$(id -un)"
CONTROLLER="${SPRINKLER_CONTROLLER:-sim}"
echo "== Sprinkler panel: installing from $DIR for user $USER_NAME"

# --- 1. Python environment -------------------------------------------------------
echo "== 1/5 Python packages"
sudo apt-get update -y
sudo apt-get install -y python3-venv curl
python3 -m venv "$DIR/hmi/.venv"
"$DIR/hmi/.venv/bin/pip" install --upgrade pip
"$DIR/hmi/.venv/bin/pip" install -r "$DIR/hmi/requirements.txt"

chmod +x "$DIR/hmi/scripts/kiosk.sh" "$DIR/hmi/scripts/kiosk-watchdog.sh"

install_unit() {  # $1 = unit file name in hmi/scripts
  sed -e "s|__USER__|$USER_NAME|g" -e "s|__DIR__|$DIR|g" -e "s|__CONTROLLER__|$CONTROLLER|g" "$DIR/hmi/scripts/$1" \
    | sudo tee "/etc/systemd/system/$1" >/dev/null
}

# --- 2. HMI server service ---------------------------------------------------------
echo "== 2/5 Server service with systemd watchdog"
install_unit sprinkler-hmi.service
echo "   controller: $CONTROLLER"
# the serial port of the controller (/dev/ttyACM0) belongs to the group dialout
sudo usermod -aG dialout "$USER_NAME"
# The panel may restart the Pi (Settings -> Restart, technician key only): allow exactly
# this one command without a password, nothing else.
SUDOERS_TMP="$(mktemp)"
echo "$USER_NAME ALL=(root) NOPASSWD: /usr/bin/systemctl reboot" > "$SUDOERS_TMP"
if sudo visudo -cf "$SUDOERS_TMP" >/dev/null; then
  sudo install -m 0440 -o root -g root "$SUDOERS_TMP" /etc/sudoers.d/sprinkler-reboot
fi
rm -f "$SUDOERS_TMP"

# --- 3. Kiosk ------------------------------------------------------------------------
echo "== 3/5 Kiosk mode"
if command -v raspi-config >/dev/null; then
  sudo raspi-config nonint do_boot_behaviour B4   # desktop, automatic login
  sudo raspi-config nonint do_blanking 1          # never blank the screen
fi
AUTOSTART="$HOME/.config/labwc/autostart"
mkdir -p "$(dirname "$AUTOSTART")"
touch "$AUTOSTART"
if ! grep -q "hmi/scripts/kiosk.sh" "$AUTOSTART"; then
  echo "$DIR/hmi/scripts/kiosk.sh &" >> "$AUTOSTART"
fi
# Also a standard desktop autostart entry, for desktops that do not use labwc
# (kiosk.sh makes sure only one copy runs).
mkdir -p "$HOME/.config/autostart"
cat > "$HOME/.config/autostart/sprinkler-kiosk.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Sprinkler panel kiosk
Exec=$DIR/hmi/scripts/kiosk.sh
X-GNOME-Autostart-enabled=true
EOF

# --- 4. Kiosk watchdog ------------------------------------------------------------
echo "== 4/5 Kiosk watchdog service"
install_unit sprinkler-kiosk-watchdog.service

# --- 5. Hardware watchdog ----------------------------------------------------------
echo "== 5/5 Hardware watchdog"
# systemd feeds the Pi's hardware watchdog; if the kernel or systemd hangs,
# the feeding stops and the chip reboots the Pi after RuntimeWatchdogSec.
sudo mkdir -p /etc/systemd/system.conf.d
sudo tee /etc/systemd/system.conf.d/sprinkler-watchdog.conf >/dev/null <<'EOF'
[Manager]
RuntimeWatchdogSec=15s
RebootWatchdogSec=2min
EOF
CONFIG=/boot/firmware/config.txt
[ -f "$CONFIG" ] || CONFIG=/boot/config.txt
if [ -f "$CONFIG" ] && ! grep -q "^dtparam=watchdog=on" "$CONFIG"; then
  echo "dtparam=watchdog=on" | sudo tee -a "$CONFIG" >/dev/null
fi

sudo systemctl daemon-reload
sudo systemctl enable sprinkler-hmi.service
sudo systemctl restart sprinkler-hmi.service
sudo systemctl enable --now sprinkler-kiosk-watchdog.service

echo
echo "== Done. Reboot the Pi:  sudo reboot"
echo "   After the reboot the panel page opens full screen by itself."
echo "   Check:  systemctl status sprinkler-hmi     (should say: active (running))"
echo "           curl -s localhost:8000/api/health  (should say: \"ok\": true)"
