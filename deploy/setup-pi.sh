#!/usr/bin/env bash
# QNU16 setup for Raspberry Pi OS (Bookworm, 64-bit) on a Pi 5, or Debian/Ubuntu on an x86 mini PC.
# Installs build deps + JACK, builds QNU16, tunes the system for low-latency audio and installs
# systemd services. Run from the repo root:  sudo deploy/setup-pi.sh
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "run with sudo" >&2; exit 1; fi
REPO="$(cd "$(dirname "$0")/.." && pwd)"
USER_NAME="${SUDO_USER:-pi}"
PREFIX=/opt/qnu16

echo "==> Packages"
apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
  build-essential cmake pkg-config jackd2 libjack-jackd2-dev alsa-utils
# Optional: not packaged on every distro.
DEBIAN_FRONTEND=noninteractive apt-get install -y cpufrequtils || echo "cpufrequtils unavailable; set the governor manually"

echo "==> Build"
sudo -u "$USER_NAME" cmake -S "$REPO" -B "$REPO/build" -DCMAKE_BUILD_TYPE=Release
sudo -u "$USER_NAME" cmake --build "$REPO/build" -j"$(nproc)"
sudo -u "$USER_NAME" "$REPO/build/qnu16-tests"
cmake --install "$REPO/build" --prefix "$PREFIX"
install -d -o "$USER_NAME" -g "$USER_NAME" /var/lib/qnu16/scenes
install -d -o "$USER_NAME" -g "$USER_NAME" /var/lib/qnu16/drive
install -d -o "$USER_NAME" -g "$USER_NAME" /var/lib/qnu16/libraries

echo "==> Realtime permissions"
usermod -aG audio "$USER_NAME"
cat > /etc/security/limits.d/95-qnu16-audio.conf <<'EOF'
@audio - rtprio 95
@audio - memlock unlimited
EOF

echo "==> CPU governor: performance (avoids clock ramp-up glitches)"
echo 'GOVERNOR="performance"' > /etc/default/cpufrequtils

# Pi 5: keep audio off CPU 3's competition. isolcpus is optional; uncomment to dedicate a core.
# sed -i 's/$/ isolcpus=3 nohz_full=3/' /boot/firmware/cmdline.txt

echo "==> Services"
install -m 644 "$REPO/deploy/jackd.service" /etc/systemd/system/qnu16-jackd.service
install -m 644 "$REPO/deploy/qnu16.service" /etc/systemd/system/qnu16.service
sed -i "s/^User=.*/User=$USER_NAME/" /etc/systemd/system/qnu16-jackd.service /etc/systemd/system/qnu16.service
[[ -f /etc/default/qnu16 ]] || install -m 644 "$REPO/deploy/qnu16.default" /etc/default/qnu16
systemctl daemon-reload
systemctl enable qnu16-jackd.service qnu16.service

cat <<EOF

Done. Next:
  1. Plug in the interface and check its ALSA name:   aplay -l
  2. Edit /etc/default/qnu16 (JACK_DEVICE, period size)
  3. Reboot (for the audio group + rtprio limits), then open http://$(hostname).local:8016/
EOF
