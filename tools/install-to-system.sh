#!/usr/bin/env bash
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO/refs/libfprint/build"

echo "==================================================="
echo "  EH577 Libfprint System Installer"
echo "==================================================="
echo ""

# Ask for sudo upfront
echo "Requesting sudo privileges for system installation..."
sudo -v

# Keep sudo alive in the background while building
while true; do sudo -n true; sleep 60; kill -0 "$$" || exit; done 2>/dev/null &

echo ""
echo "[1/4] Syncing local driver modifications to build tree..."
# Just in case you edited the wip-libfprint files directly, sync them over
cp "$REPO/wip-libfprint/egis0577.c" "$REPO/refs/libfprint/libfprint/drivers/egis0577.c" 2>/dev/null || true
cp "$REPO/wip-libfprint/egis0577.h" "$REPO/refs/libfprint/libfprint/drivers/egis0577.h" 2>/dev/null || true

echo "[2/4] Configuring meson for system prefix (/usr)..."
# Configure to overwrite the actual system libfprint
meson configure "$BUILD_DIR" --prefix=/usr --libdir=lib/x86_64-linux-gnu

echo "[3/4] Building and installing libfprint..."
ninja -C "$BUILD_DIR"
sudo ninja -C "$BUILD_DIR" install

echo "[4/4] Restarting fprintd service..."
sudo systemctl restart fprintd

echo ""
echo "✅ Driver successfully built, installed, and fprintd restarted!"
