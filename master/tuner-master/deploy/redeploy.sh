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
# Optional flag:
#
#   --push-config <path>   Push <path> to /etc/tuner-master/config.toml on
#                          the Pi (backing up the existing one to .bak),
#                          then restart the service. WITHOUT this flag the
#                          Pi-side config is NEVER touched — your
#                          hand-edited [tuner].host, [cat].device, etc.
#                          survive every redeploy.
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

PUSH_CONFIG_FILE=""

usage() {
    echo "Usage: $0 [--push-config <path>] [user@host[:port]]"
}

# Parse leading flags, then the optional positional [user@]host[:port].
while [ "${1:-}" != "" ]; do
    case "$1" in
        --push-config)
            shift
            if [ -z "${1:-}" ]; then
                echo "--push-config requires a file path." >&2
                usage
                exit 1
            fi
            PUSH_CONFIG_FILE="$1"
            if [ ! -f "$PUSH_CONFIG_FILE" ]; then
                echo "--push-config: file not found: $PUSH_CONFIG_FILE" >&2
                exit 1
            fi
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --*)
            echo "Unknown flag: $1" >&2
            usage
            exit 1
            ;;
        *)
            # Positional [user@]host[:port].
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
            ;;
    esac
    shift
done

echo "Target: ${PI_USER}@${PI_HOST}:${PI_PORT}"
if [ -n "$PUSH_CONFIG_FILE" ]; then
    echo "Config: WILL push '${PUSH_CONFIG_FILE}' → /etc/tuner-master/config.toml (existing → .bak)"
else
    echo "Config: preserved on the Pi (use --push-config <path> to overwrite)"
fi

REMOTE="${PI_USER}@${PI_HOST}"
BINARY="dist/tuner-master-linux-arm64"

# SSH ControlMaster: open a single multiplexed connection so subsequent
# ssh/scp invocations share auth. If the user doesn't have keys set up,
# this collapses 5-8 password prompts down to 1.
SSH_CTRL_PATH="${TMPDIR:-/tmp}/redeploy-ssh-$$.sock"
SSH_CTRL_OPTS=(
    -o ControlMaster=auto
    -o ControlPath="$SSH_CTRL_PATH"
    -o ControlPersist=60s
)
SSH_OPTS=(-p "$PI_PORT" -o ConnectTimeout=10 "${SSH_CTRL_OPTS[@]}")
SCP_OPTS=(-P "$PI_PORT" -o ConnectTimeout=10 "${SSH_CTRL_OPTS[@]}")

# Tear down the multiplexed connection when the script exits.
cleanup_ssh() {
    if [ -e "$SSH_CTRL_PATH" ]; then
        ssh -o ControlPath="$SSH_CTRL_PATH" -O exit "$REMOTE" 2>/dev/null || true
    fi
}
trap cleanup_ssh EXIT

push_config_if_requested() {
    if [ -z "$PUSH_CONFIG_FILE" ]; then
        return 0
    fi
    local stage="/tmp/tuner-master.cfg.new"
    echo "       --push-config: scp $(basename "$PUSH_CONFIG_FILE") → ${REMOTE}:${stage}..."
    scp "${SCP_OPTS[@]}" "$PUSH_CONFIG_FILE" "${REMOTE}:${stage}"
    ssh "${SSH_OPTS[@]}" "$REMOTE" "
        set -e
        if [ -f /etc/tuner-master/config.toml ]; then
            sudo cp -a /etc/tuner-master/config.toml /etc/tuner-master/config.toml.bak
            echo '         backed up existing config → /etc/tuner-master/config.toml.bak'
        fi
        sudo install -m 640 -o root -g tuner '${stage}' /etc/tuner-master/config.toml
        rm -f '${stage}'
        sudo systemctl restart tuner-master.service
    "
}

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

    echo "       atomic install + restart (Pi-side config NOT touched)..."
    ssh "${SSH_OPTS[@]}" "$REMOTE" "
        set -e
        sudo install -m 755 -o root -g root '${REMOTE_STAGE}' /opt/tuner-master/tuner-master
        rm -f '${REMOTE_STAGE}'
        sudo systemctl restart tuner-master.service
        sudo systemctl --no-pager --lines=5 status tuner-master.service
    "
    push_config_if_requested
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
        sudo '${REMOTE_DIR}/install.sh' '${REMOTE_DIR}/dist/tuner-master-linux-arm64'
        rm -rf '${REMOTE_DIR}'
    "
    # On first install, install.sh has just seeded /etc/tuner-master/config.toml
    # from config.example.toml. If --push-config was requested, overwrite
    # that seed now (and back it up). Subsequent redeploys will leave the
    # config alone unless --push-config is set again.
    push_config_if_requested
    if [ -z "$PUSH_CONFIG_FILE" ]; then
        echo "       NOTE: edit /etc/tuner-master/config.toml on the Pi to point at your tuner controller IP,"
        echo "             then 'sudo systemctl restart tuner-master.service'."
    fi
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
