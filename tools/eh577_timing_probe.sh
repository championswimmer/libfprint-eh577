#!/usr/bin/env bash
# EH577 press-timing probe wrapper.
# Run as:  ./tools/eh577_timing_probe.sh
# The script self-escalates to sudo and sets up the live-frame path so the
# C binary can snapshot the driver's current processed frame on 'r'.
set -euo pipefail

# ── self-escalate to root ────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    exec sudo "$0" "$@"
fi

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_EXAMPLES="$REPO/refs/libfprint/build/examples"
LIBFP="$REPO/refs/libfprint/build/libfprint"
PROBE_BIN="$BUILD_EXAMPLES/eh577-timing-probe"

if [[ ! -x "$PROBE_BIN" ]]; then
    echo "ERROR: $PROBE_BIN not found. Build with: cd refs/libfprint/build && ninja"
    exit 1
fi

SESSION="$(date +%Y%m%d-%H%M%S)"
SESS_DIR="$REPO/artifacts/timing-probe/$SESSION"
LIVE_FRAME="$SESS_DIR/live-frame.pgm"

mkdir -p "$SESS_DIR"

# ── free the USB device ──────────────────────────────────────────────────────
free_device() {
    systemctl stop fprintd fprintd.socket 2>/dev/null || true
    pkill -f fprintd 2>/dev/null || true
    pkill -f "eh577-" 2>/dev/null || true
    local devnode
    devnode=$(lsusb -d 1c7a:0577 2>/dev/null \
        | awk '{printf "/dev/bus/usb/%s/%s\n", $2, $4}' | tr -d ':')
    [[ -z "$devnode" || ! -e "$devnode" ]] && return
    for i in 1 2 3 4 5; do
        fuser "$devnode" &>/dev/null || break
        echo "Waiting for USB device to be released (${i}s)..."
        sleep 1
    done
}

# ── chown artifacts back to the invoking user on exit ───────────────────────
cleanup() {
    local owner="${SUDO_USER:-}"
    [[ -n "$owner" ]] && chown -R "$owner:$owner" "$SESS_DIR" 2>/dev/null || true
}
trap cleanup EXIT

# ── preflight ────────────────────────────────────────────────────────────────
if ! lsusb | grep -q '1c7a:0577'; then
    echo "ERROR: EH577 not found via lsusb"
    exit 1
fi

free_device

echo "Session : $SESS_DIR"
echo "Live PGM: $LIVE_FRAME  (updated each frame; copied on 'r')"
echo ""

# ── launch probe ─────────────────────────────────────────────────────────────
EGIS0577_LIVE_FRAME_PATH="$LIVE_FRAME" \
G_MESSAGES_DEBUG=libfprint,libfprint-egis0577 \
LD_LIBRARY_PATH="$LIBFP" \
"$PROBE_BIN" "$SESS_DIR"
