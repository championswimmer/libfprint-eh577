#!/usr/bin/env bash
# Shared helpers for the EH577 probe saturation tests.
#
# Each test-*.sh script sources this, then calls:
#   probe_prep                      # find device, build probe, cache sudo, stop fprintd
#   probe_preinit "<dump_dir>"      # optional: arm with pre-init (no finger needed)
#   probe_guided_capture "<dump_dir>" "<probe-mode>"   # finger-hold single-frame capture
#   probe_report "<dump_dir>"       # chown dumps + print the 64 14 ec frame path
#
# Goal: capture one 5356-byte `64 14 ec` finger frame per probe mode so saturation
# can be compared against the driver's frames. See
# docs/research-log/2026-06-12/06-probe-vs-driver-init-diff.md

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROBE="$REPO/build/eh577_usbfs_probe"
PROBE_SRC="$REPO/tools/eh577_usbfs_probe.c"
GUIDED="$REPO/tools/eh577_guided_capture.sh"
USBDEV=""

say() { echo "$@"; }

probe_find_device() {
  # Parse `lsusb` for the EH577 and build its /dev/bus/usb path.
  local line bus dev
  line="$(lsusb 2>/dev/null | grep -i '1c7a:0577' | head -1 || true)"
  if [[ -z "$line" ]]; then
    say "ERROR: EH577 (1c7a:0577) not found on USB. Check the connection."
    exit 1
  fi
  bus="$(awk '{print $2}' <<<"$line")"
  dev="$(awk '{gsub(":","",$4); print $4}' <<<"$line")"
  USBDEV="/dev/bus/usb/$bus/$dev"
  if [[ ! -e "$USBDEV" ]]; then
    say "ERROR: device node $USBDEV does not exist."
    exit 1
  fi
  say "EH577 at $USBDEV"
}

probe_build() {
  if [[ ! -x "$PROBE" || "$PROBE_SRC" -nt "$PROBE" ]]; then
    say "Building probe..."
    mkdir -p "$(dirname "$PROBE")"
    gcc -O2 -Wall -o "$PROBE" "$PROBE_SRC"
  fi
}

probe_prep() {
  probe_build
  probe_find_device
  say "Caching sudo credentials (you may be prompted once)..."
  sudo -v
  # Free the device from fprintd if it is holding it.
  sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
  sudo pkill -f fprintd 2>/dev/null || true
}

# Arm the sensor with the pre-init sequence (no finger needed).
probe_preinit() {
  local dump_dir="$1"
  mkdir -p "$dump_dir"
  say "Running pre-init (29 packets) to arm the sensor..."
  sudo -n env EH577_DUMP_DIR="$dump_dir" "$PROBE" "$USBDEV" eh575-preinit 29 >/dev/null 2>&1 || true
}

# Single-frame capture while the finger is held, via the guided timing helper.
probe_guided_capture() {
  local dump_dir="$1" mode="$2"
  mkdir -p "$dump_dir"
  say ""
  say "Capture: probe mode '$mode' -> $dump_dir"
  say "Press and HOLD your left thumb firmly when prompted, and keep holding."
  "$GUIDED" \
    --delay-before-start 3 \
    --delay-before-touch 1 \
    --hold 10 \
    --delay-after-remove 2 \
    --cycles 1 \
    -- sudo -n bash -lc "export EH577_DUMP_DIR='$dump_dir'; '$PROBE' '$USBDEV' $mode 18"
}

probe_report() {
  local dump_dir="$1"
  sudo chown -R "$(id -u):$(id -g)" "$dump_dir" 2>/dev/null || true
  say ""
  say "=== done ==="
  say "Dump dir: $dump_dir"
  local frame
  frame="$(ls "$dump_dir"/*64_14_ec*.bin 2>/dev/null | tail -1 || true)"
  if [[ -n "$frame" ]]; then
    say "Frame:    $frame ($(stat -c%s "$frame") bytes)"
  else
    say "WARNING: no 64 14 ec frame was dumped — was the finger down during capture?"
  fi
  say "Share this dump dir path in the chat so the saturation can be analyzed."
}
