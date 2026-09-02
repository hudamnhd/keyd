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

echo "[2/7] Applying patches..."

echo "  - X11"
git apply "$PROJECT_DIR/patches/X11.patch"

echo "  - Instantwmctrl mod state"
git apply "$PROJECT_DIR/patches/instantwmctrlmodstate.patch"

echo "  - Multilayer"
git apply "$PROJECT_DIR/patches/multilayer.patch"

echo "[3/7] Cleaning..."
make clean

echo "[4/7] Building..."
make

echo "[5/7] Installing..."
sudo make install X11_ENV=1

echo "[6/8] Restarting keyd..."
sudo pkill keyd || true
sleep 1
sudo systemctl enable --now keyd

sleep 1

echo "Checking keyd status..."
sudo systemctl status keyd --no-pager

sleep 1
echo "[6/8] Xset r rate"
xset r rate 200 50 &

echo "Done."
