#!/usr/bin/env bash
# Install the tuner-master service on a Raspberry Pi (64-bit Raspberry Pi OS
# / Debian 12+ derivative). Run AS ROOT on the Pi after copying the deploy/
# directory and the freshly-built binary there.
#
# Layout this installs:
#   /opt/tuner-master/tuner-master            (the binary)
#   /etc/tuner-master/config.toml             (config, edit before first start)
#   /var/lib/tuner-master/                    (SQLite memory DB, runtime state)
#   /etc/systemd/system/tuner-master.service  (systemd unit)
#   user/group: tuner (system account, no shell)
#
# Idempotent — safe to re-run for upgrades. Existing config is never
# overwritten; the example is copied only when no config exists yet.
#
# Usage (on the Pi):
#   sudo ./install.sh [BINARY_PATH]
#
# BINARY_PATH defaults to the linux/arm64 binary in dist/ if it exists,
# otherwise looks next to this script.

set -euo pipefail

if [ "$(id -u)" != "0" ]; then
    echo "install.sh must be run as root (use sudo)." >&2
    exit 1
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"

# Resolve the binary path.
BINARY="${1:-}"
if [ -z "$BINARY" ]; then
    for candidate in \
        "$SCRIPT_DIR/../dist/tuner-master-linux-arm64" \
        "$SCRIPT_DIR/../dist/tuner-master-linux-armv7" \
        "$SCRIPT_DIR/tuner-master"; do
        if [ -x "$candidate" ]; then
            BINARY="$candidate"
            break
        fi
    done
fi
if [ -z "$BINARY" ] || [ ! -x "$BINARY" ]; then
    echo "Binary not found. Pass the path as the first argument," >&2
    echo "or run deploy/build-pi.sh on the dev machine first and copy" >&2
    echo "the dist/ folder + this deploy/ folder to the Pi." >&2
    exit 1
fi

echo "Installing tuner-master from: $BINARY"

# 1) System user/group.
if ! id -u tuner &>/dev/null; then
    echo "[1/6] creating system user 'tuner'..."
    useradd \
        --system \
        --user-group \
        --no-create-home \
        --shell /usr/sbin/nologin \
        tuner
else
    echo "[1/6] user 'tuner' already exists — skipping"
fi

# 2) Directory layout.
echo "[2/6] creating /opt/tuner-master, /etc/tuner-master, /var/lib/tuner-master..."
install -d -m 755 -o root  -g root  /opt/tuner-master
install -d -m 755 -o root  -g root  /etc/tuner-master
install -d -m 750 -o tuner -g tuner /var/lib/tuner-master

# 3) Stop service if upgrading.
if systemctl is-active --quiet tuner-master.service 2>/dev/null; then
    echo "[3/6] stopping running tuner-master.service for upgrade..."
    systemctl stop tuner-master.service
else
    echo "[3/6] no running service to stop"
fi

# 4) Binary.
echo "[4/6] installing binary → /opt/tuner-master/tuner-master..."
install -m 755 -o root -g root "$BINARY" /opt/tuner-master/tuner-master

# 5) Config (only on first install — never overwrite).
if [ -f /etc/tuner-master/config.toml ]; then
    echo "[5/6] config already exists at /etc/tuner-master/config.toml — keeping"
else
    echo "[5/6] installing config example → /etc/tuner-master/config.toml..."
    install -m 644 -o root -g root \
        "$SCRIPT_DIR/config.example.toml" /etc/tuner-master/config.toml
    chown root:tuner /etc/tuner-master/config.toml
    chmod 640 /etc/tuner-master/config.toml
    echo "       EDIT /etc/tuner-master/config.toml BEFORE STARTING THE SERVICE."
fi

# 6) systemd unit + reload + enable + start.
echo "[6/6] installing systemd unit and starting service..."
install -m 644 -o root -g root \
    "$SCRIPT_DIR/tuner-master.service" \
    /etc/systemd/system/tuner-master.service
systemctl daemon-reload
systemctl enable tuner-master.service
systemctl restart tuner-master.service

echo
echo "Install complete."
echo
echo "Status:"
systemctl --no-pager --lines=10 status tuner-master.service || true
echo
echo "Tail the logs with:"
echo "   journalctl -u tuner-master.service -f"
echo
echo "Edit the config with:"
echo "   sudo nano /etc/tuner-master/config.toml"
echo "Then:"
echo "   sudo systemctl restart tuner-master.service"
