#!/usr/bin/env bash
# Cross-compile the tuner-master binary for the Raspberry Pi.
#
# Default is linux/arm64 (Pi 3/4/5 with 64-bit Raspberry Pi OS, which is
# the recommended posture). For 32-bit OS / Pi Zero set ARCH=arm.
#
# Pure-Go module (modernc.org/sqlite when added at M3, gorilla/websocket,
# BurntSushi/toml are all CGO-free), so no cross-toolchain is needed —
# any Go installation that can target linux/arm64 works.
#
# Usage:
#   ./deploy/build-pi.sh                # → dist/tuner-master-linux-arm64
#   ARCH=arm  ./deploy/build-pi.sh      # → dist/tuner-master-linux-armv7

set -euo pipefail

# Resolve script directory and cd to the Go module root.
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
MODULE_DIR="$(dirname "$SCRIPT_DIR")"
cd "$MODULE_DIR"

ARCH="${ARCH:-arm64}"
case "$ARCH" in
    arm64)
        GOARCH=arm64
        SUFFIX=arm64
        unset GOARM
        ;;
    arm|armv7)
        GOARCH=arm
        GOARM=7
        SUFFIX=armv7
        ;;
    *)
        echo "Unknown ARCH=$ARCH (expected: arm64 | arm | armv7)" >&2
        exit 1
        ;;
esac

mkdir -p dist
OUT="dist/tuner-master-linux-${SUFFIX}"

VERSION="$(git describe --tags --dirty --always 2>/dev/null || echo "dev")"
BUILD_TIME="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

echo "Cross-compiling tuner-master..."
echo "  GOOS=linux GOARCH=${GOARCH}${GOARM:+ GOARM=$GOARM}"
echo "  version: ${VERSION}"
echo "  output:  ${OUT}"

export GOOS=linux
export GOARCH="$GOARCH"
export CGO_ENABLED=0
if [ -n "${GOARM:-}" ]; then
    export GOARM
fi

go build \
    -trimpath \
    -ldflags "-s -w -X main.version=${VERSION} -X main.buildTime=${BUILD_TIME}" \
    -o "$OUT" \
    .

SIZE=$(du -h "$OUT" | cut -f1)
echo "Built ${OUT} (${SIZE})"
