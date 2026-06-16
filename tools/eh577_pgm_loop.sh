#!/usr/bin/env bash
# Lean PGM capture loop — no cue timers, no finger-selection UI.
# Each accepted press is saved as a numbered PGM and counted.
#
# Usage: ./tools/eh577_pgm_loop.sh [N]    # default N=5
#
# Accepted PGMs land in artifacts/pgm/YYYYMMDD-HHMMSS-NNN.pgm.
# Exit 0 if at least one frame was accepted; exit 1 if all were rejected.

set -uo pipefail

if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  exec sudo "$0" "$@"
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
HELPER="$REPO/refs/libfprint/build/examples/eh577-capture-helper"
COUNT="${1:-5}"
SESSION="$(date +%Y%m%d-%H%M%S)"
TMP_DIR="$(mktemp -d)"
PGM_DIR="$REPO/artifacts/pgm"
LOG="$TMP_DIR/pgm-loop.log"

trap 'rm -rf "$TMP_DIR"' EXIT

if [[ ! -x "$HELPER" ]]; then
  echo "Helper not built: $HELPER"; exit 1
fi
if ! lsusb | grep -q '1c7a:0577'; then
  echo "EH577 (1c7a:0577) not on USB — replug it."; exit 1
fi

systemctl stop fprintd fprintd.socket 2>/dev/null || true
pkill -f fprintd 2>/dev/null || true

mkdir -p "$PGM_DIR"

echo "Capturing $COUNT frame(s) — touch the sensor when prompted."
LD_LIBRARY_PATH="$LIBFP" \
G_MESSAGES_DEBUG=libfprint-egis0577 \
  "$HELPER" "$TMP_DIR" "$COUNT" 2>"$LOG"

# Rename and move accepted PGMs with session-scoped sequence numbers.
accepted=0
for pgm in "$TMP_DIR"/capture-*.pgm; do
  [[ -e "$pgm" ]] || continue
  seq=$(printf "%03d" $((accepted + 1)))
  dest="$PGM_DIR/${SESSION}-${seq}.pgm"
  cp "$pgm" "$dest"
  accepted=$((accepted + 1))
  echo "  saved: $dest"
done

rejected=$((COUNT - accepted))

# Print stage-2 accept/reject summary from driver log.
echo ""
echo "--- Stage-2 summary (from driver log) ---"
grep -E "Stage-2 (accept|reject)" "$LOG" | sed 's/^/  /' || true
echo ""
echo "Frames requested: $COUNT"
echo "Accepted:         $accepted"
echo "Rejected:         $rejected"

if [[ -n "${SUDO_UID:-}" ]]; then
  chown -R "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" "$PGM_DIR" 2>/dev/null || true
fi

[[ $accepted -ge 1 ]]
