#!/usr/bin/env bash
# Capture raw frames with NO finger on the sensor, then analyse them.
# Purpose: provide real "no-finger" data so EGIS0577_MIN_SD (or a future
# replacement heuristic) can be tuned against measured values rather than
# guesses.
#
# Usage:
#   tools/capture_phantom_frames.sh [duration_seconds]
#
# The script runs an identify scan session for DURATION seconds while you keep
# your hands off the sensor, then prints statistics for every frame that made
# it past the driver's valid_data() check (nonzero > 0).  If no frames appear,
# the driver is correctly rejecting idle data.
#
# Output files land in:
#   dumps/phantom-<timestamp>/
# Each file is named:
#   <seq>-<pktarray>-nonzero-<N>.bin

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
IDENTIFY="$REPO/refs/libfprint/build/examples/identify"
DURATION="${1:-30}"
SESSION="$(date +%Y%m%d-%H%M%S)"
DUMPDIR="$REPO/dumps/phantom-$SESSION"

mkdir -p "$DUMPDIR"
echo ""
echo "════════════════════════════════════════"
echo "  EH577 Phantom-Frame Capture"
echo "════════════════════════════════════════"
echo ""
echo "Do NOT touch the sensor for the next $DURATION seconds."
echo "Frames that pass the driver's filter will land in:"
echo "  $DUMPDIR"
echo ""

if ! lsusb | grep -q '1c7a:0577'; then
  echo "ERROR: EH577 not found."; exit 1
fi
if systemctl is-active --quiet fprintd 2>/dev/null; then
  echo "Stopping fprintd..."; sudo systemctl stop fprintd
fi
sudo bash -c 'echo "on" > /sys/bus/usb/devices/3-3/power/control' 2>/dev/null || true

for i in $(seq "$DURATION" -1 1); do printf "  Scanning... %2d s\r" "$i"; sleep 1; done
echo ""

# Run identify binary with frame dumping; feed 'n' so it loops back without
# waiting for stdin at the end.
( printf "n\n%.0s" {1..100} ) | sudo sh -c "
  cd '$REPO'
  exec env LD_LIBRARY_PATH='$LIBFP' \
           G_MESSAGES_DEBUG=libfprint-egis0577 \
           EGIS0577_FRAME_DUMP_DIR='$DUMPDIR' \
           '$IDENTIFY'
" &>/dev/null &
BPID=$!

sleep "$DURATION"
kill "$BPID" 2>/dev/null || true
sudo chown -R "$(id -u):$(id -g)" "$DUMPDIR" 2>/dev/null || true

echo ""
echo "Capture complete.  Analysing frames in $DUMPDIR ..."
echo ""

# Analyse captured frames
python3 - "$DUMPDIR" <<'PYEOF'
import sys, os, struct

dump_dir = sys.argv[1]
files = sorted(f for f in os.listdir(dump_dir) if f.endswith('.bin'))

if not files:
    print("  No frames captured — driver correctly rejected all idle frames.")
    print("  (If phantom matches are still occurring, they may be from brief")
    print("   accidental touches rather than true no-finger phantom frames.)")
    sys.exit(0)

print(f"  WARNING: {len(files)} frame(s) passed the driver filter with no finger!")
print()
for fname in files:
    path = os.path.join(dump_dir, fname)
    with open(path, 'rb') as fh:
        data = fh.read()
    vals = list(data)
    nonzero = [v for v in vals if v > 0]
    mean_all = sum(vals) / len(vals)
    # variance
    sq_diff = sum((v - mean_all)**2 for v in vals)
    variance = sq_diff / len(vals)
    sd = variance ** 0.5
    mean_nz = sum(nonzero) / len(nonzero) if nonzero else 0
    above10 = sum(1 for v in vals if v > 10)
    above20 = sum(1 for v in vals if v > 20)
    print(f"  {fname}")
    print(f"    size={len(data)}  nonzero={len(nonzero)}  mean={mean_all:.2f}  SD={sd:.2f}  var={variance:.2f}")
    print(f"    mean(nonzero)={mean_nz:.1f}  >10={above10}  >20={above20}")
    print()
PYEOF
