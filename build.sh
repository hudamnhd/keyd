#!/bin/bash

set -e

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$(mktemp -d)"

cleanup() {
    rm -rf "$BUILD_DIR"
}

trap cleanup EXIT

echo "[1/7] Preparing temporary build..."
cp -a "$PROJECT_DIR/." "$BUILD_DIR/"
cd "$BUILD_DIR"

echo "[2/7] Cleaning..."
make clean

echo "[3/7] Building..."
make

echo "[4/7] Installing..."
sudo make install X11_ENV=1

echo "[5/7] Reloading systemd and restarting keyd..."

sudo systemctl daemon-reload

sudo systemctl stop keyd || true

sleep 1

if pgrep -x keyd >/dev/null; then
    echo "keyd is still running, terminating leftover process..."
    sudo pkill -x keyd || true
    sleep 1
fi

sudo systemctl start keyd

if ! sudo systemctl is-active --quiet keyd; then
    echo "ERROR: keyd failed to start."
    sudo systemctl --no-pager --full status keyd
    exit 1
fi

echo "keyd is active."

sleep 1

echo "[6/7] Configuring X11 key repeat rate..."
xset r rate 200 50 &

sleep 1

echo "[7/7] Starting overlay..."
keyd overlay-start

echo
echo "Done."
