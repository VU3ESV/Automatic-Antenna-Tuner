#!/usr/bin/env bash
# shellcheck disable=SC2029  # local vars (REMOTE_STAGE, REMOTE_DIR) are
#                             # constants we WANT expanded client-side
#                             # before being sent over ssh.

# Build a fresh linux/arm64 binary on the dev machine, push it to the Pi,
# and either:
#   * if tuner-master.service is already installed on the Pi → fast
#     redeploy (swap binary + restart), or
#   * if not → stage deploy/ on the Pi and run install.sh end-to-end.
#
# That makes this the only script you need after the first push: it
# bootstraps a fresh Pi and updates an existing one with the same
# command.
#
# Target the Pi one of two ways:
#
#   1. Positional argument:   ./deploy/redeploy.sh pi@tuner-pi.local
#                             ./deploy/redeploy.sh vinod@192.168.1.42:2222
#      Accepts the standard SSH spec [user@]host[:port].
#
#   2. Env vars:              PI_HOST, PI_USER, PI_PORT (export in your
#                             shell rc, or pass inline). Used when no
#                             positional arg is given.
#
#      defaults: PI_USER=pi  PI_HOST=tuner-pi.local  PI_PORT=22
#
# The Pi user must have passwordless sudo (the usual Pi default).
# Requires: ssh, scp, sudo on the Pi.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
cd "$MODULE_DIR"

# Defaults (overridden by env vars, then by the positional arg).
PI_HOST="${PI_HOST:-tuner-pi.local}"
PI_USER="${PI_USER:-pi}"
PI_PORT="${PI_PORT:-22}"

# Positional arg overrides env vars: [user@]host[:port].
if [ -n "${1:-}" ]; then
    spec="$1"
    if [[ "$spec" == *@* ]]; then
        PI_USER="${spec%%@*}"
        spec="${spec#*@}"
    fi
    if [[ "$spec" == *:* ]]; then
        PI_PORT="${spec##*:}"
        spec="${spec%:*}"
    fi
    PI_HOST="$spec"
fi
echo "Target: ${PI_USER}@${PI_HOST}:${PI_PORT}"

REMOTE="${PI_USER}@${PI_HOST}"
BINARY="dist/tuner-master-linux-arm64"
SSH_OPTS=(-p "$PI_PORT" -o ConnectTimeout=10)
SCP_OPTS=(-P "$PI_PORT" -o ConnectTimeout=10)

# 1) Cross-compile (idempotent; cheap re-run).
echo "[1/4] Cross-compiling..."
"$SCRIPT_DIR/build-pi.sh"

# 2) Detect whether the service is already installed on the Pi.
#    `systemctl list-unit-files` exits 0 even when the unit isn't found,
#    so we grep the output instead.
echo "[2/4] Probing ${REMOTE} for an existing tuner-master.service..."
if ssh "${SSH_OPTS[@]}" "$REMOTE" \
       "systemctl list-unit-files tuner-master.service --no-pager 2>/dev/null \
        | grep -q '^tuner-master.service'"; then
    MODE=redeploy
    echo "       service IS installed → fast redeploy (swap binary + restart)"
else
    MODE=install
    echo "       service NOT installed → full install.sh on the Pi"
fi

# 3) Push.
if [ "$MODE" = "redeploy" ]; then
    REMOTE_STAGE="/tmp/tuner-master.new"
    echo "[3/4] scp $(basename "$BINARY") → ${REMOTE}:${REMOTE_STAGE}..."
    scp "${SCP_OPTS[@]}" "$BINARY" "${REMOTE}:${REMOTE_STAGE}"

    echo "       atomic install + restart..."
    ssh "${SSH_OPTS[@]}" "$REMOTE" "
        set -e
        sudo install -m 755 -o root -g root '${REMOTE_STAGE}' /opt/tuner-master/tuner-master
        rm -f '${REMOTE_STAGE}'
        sudo systemctl restart tuner-master.service
        sudo systemctl --no-pager --lines=5 status tuner-master.service
    "
else
    # First-time install: stage the entire deploy/ folder plus the binary,
    # then let install.sh do the heavy lifting.
    REMOTE_DIR="/tmp/tuner-master-bootstrap"
    echo "[3/4] staging deploy/ + binary → ${REMOTE}:${REMOTE_DIR}/..."
    ssh "${SSH_OPTS[@]}" "$REMOTE" "rm -rf '${REMOTE_DIR}' && mkdir -p '${REMOTE_DIR}/dist'"
    # The trailing /. on the source path copies contents (not the directory itself).
    scp "${SCP_OPTS[@]}" -r "${SCRIPT_DIR}/." "${REMOTE}:${REMOTE_DIR}/"
    scp "${SCP_OPTS[@]}" "$BINARY" "${REMOTE}:${REMOTE_DIR}/dist/"

    echo "       running install.sh on the Pi (sudo will prompt if not passwordless)..."
    ssh "${SSH_OPTS[@]}" "$REMOTE" "
        set -e
        chmod +x '${REMOTE_DIR}/install.sh' '${REMOTE_DIR}/build-pi.sh' '${REMOTE_DIR}/redeploy.sh' 2>/dev/null || true
        sudo '${REMOTE_DIR}/install.sh'
        rm -rf '${REMOTE_DIR}'
    "
    echo "       NOTE: edit /etc/tuner-master/config.toml on the Pi to point at your tuner controller IP,"
    echo "             then 'sudo systemctl restart tuner-master.service'."
fi

# 4) Health check (works for both paths).
echo "[4/4] health check..."
if curl -fsS --max-time 5 "http://${PI_HOST}:8088/healthz" >/dev/null 2>&1; then
    echo "       /healthz: ok"
else
    echo "       /healthz: NOT reachable on port 8088 yet."
    if [ "$MODE" = "install" ]; then
        echo "       (expected on first install until you edit the config and restart)"
    else
        echo "       check the journal: ssh ${REMOTE} 'journalctl -u tuner-master.service -n 50'"
        exit 1
    fi
fi

echo
echo "Done (${MODE})."
echo "Tail logs:  ssh ${REMOTE} 'journalctl -u tuner-master.service -f'"
