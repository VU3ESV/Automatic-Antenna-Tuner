#!/usr/bin/env bash
# Build a fresh linux/arm64 binary on the dev machine, scp it to the Pi,
# and restart the systemd service. This is the every-day iteration
# script — install.sh is for first-time setup.
#
# Configure once via env vars (export them in your shell rc, or pass
# inline):
#
#   PI_HOST   — hostname or IP of the Pi (e.g. "tuner-pi.local", "192.168.1.42")
#               default: tuner-pi.local
#   PI_USER   — SSH user (must have passwordless sudo, e.g. via /etc/sudoers.d)
#               default: pi
#   PI_PORT   — SSH port; default: 22
#
# Usage:
#   ./deploy/redeploy.sh
#   PI_HOST=192.168.1.42 PI_USER=vinod ./deploy/redeploy.sh
#
# Requires: ssh, scp, sudo on the Pi. First-time setup must have run
# install.sh on the Pi already.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
cd "$MODULE_DIR"

PI_HOST="${PI_HOST:-tuner-pi.local}"
PI_USER="${PI_USER:-pi}"
PI_PORT="${PI_PORT:-22}"

REMOTE="${PI_USER}@${PI_HOST}"
BINARY="dist/tuner-master-linux-arm64"
REMOTE_STAGE="/tmp/tuner-master.new"

# 1) Cross-compile.
echo "[1/4] Cross-compiling..."
"$SCRIPT_DIR/build-pi.sh"

# 2) Copy to a staging path on the Pi (avoid overwriting the running binary).
echo "[2/4] scp $(basename "$BINARY") → ${REMOTE}:${REMOTE_STAGE}..."
scp -P "$PI_PORT" "$BINARY" "${REMOTE}:${REMOTE_STAGE}"

# 3) Atomic swap + restart.
echo "[3/4] swapping binary and restarting service..."
ssh -p "$PI_PORT" "$REMOTE" "
    set -e
    sudo install -m 755 -o root -g root '${REMOTE_STAGE}' /opt/tuner-master/tuner-master
    rm -f '${REMOTE_STAGE}'
    sudo systemctl restart tuner-master.service
    sudo systemctl --no-pager --lines=5 status tuner-master.service
"

# 4) Show health endpoint.
echo "[4/4] health check..."
if curl -fsS --max-time 5 "http://${PI_HOST}:8088/healthz" >/dev/null 2>&1; then
    echo "       /healthz: ok"
else
    echo "       /healthz: NOT reachable on port 8088 — check the config and journal"
    exit 1
fi

echo
echo "Redeploy complete. Tail logs with:"
echo "  ssh ${REMOTE} 'journalctl -u tuner-master.service -f'"
