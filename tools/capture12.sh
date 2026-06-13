#!/usr/bin/env bash
# Capture N fingerprint images (default 12), one per finger touch, into a
# timestamped directory. Each image is the *processed* output the driver produces
# (rolling baseline subtraction + resize) via fp_device_capture() — the same data
# the matcher sees. Renders a 3x PNG of each for easy eyeballing and prints simple
# noise/saturation stats.
#
# Usage: ./tools/capture12.sh [count]   (default 12)

set -uo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
LIBFP="$REPO/refs/libfprint/build/libfprint"
HELPER="$REPO/refs/libfprint/build/examples/eh577-capture-helper"
COUNT="${1:-12}"
SESSION="$(date +%Y%m%d-%H%M%S)"
OUT="$REPO/artifacts/capture12/$SESSION"
LOG="$OUT/debug.log"
DEBUG_DOMAINS="${G_MESSAGES_DEBUG:-}"

mkdir -p "$OUT"
: > "$LOG"

if [[ ! -x "$HELPER" ]]; then
  echo "ERROR: capture helper not built ($HELPER)"
  echo "Build it with: meson compile -C refs/libfprint/build"
  exit 1
fi
if ! lsusb | grep -q '1c7a:0577'; then
  echo "ERROR: EH577 (1c7a:0577) not found on USB."; exit 1
fi

echo "Output dir: $OUT"
echo "Caching sudo (you may be prompted once)..."
sudo -v
# Free the device from fprintd or any stale prior helper if it is holding it.
sudo systemctl stop fprintd fprintd.socket 2>/dev/null || true
sudo pkill -f fprintd 2>/dev/null || true
sudo pkill -f "$HELPER" 2>/dev/null || true

echo ""
echo "══════════════════════════"
echo "  CAPTURE $COUNT IMAGES"
echo "══════════════════════════"
echo "Touch the SAME finger each time. Press firmly, hold until 'saved', then lift."
echo "(driver debug → $LOG; G_MESSAGES_DEBUG=$DEBUG_DOMAINS)"
echo ""

# Respect the caller's G_MESSAGES_DEBUG if set; default to quiet. Route the
# helper's combined output through a classifier so only human-facing capture
# status stays on the terminal; everything else goes to debug.log. Use
# G_MESSAGES_DEBUG=all only for deep debugging; it can create very large logs.
#
# Important: run through `sudo env ...` so caller-provided Stage-2 overrides
# actually reach the privileged helper process.
sudo env \
  LD_LIBRARY_PATH="$LIBFP" \
  G_MESSAGES_DEBUG="$DEBUG_DOMAINS" \
  EGIS0577_FRAME_DUMP_DIR="$OUT/raw" \
  EGIS0577_STAGE2_GRAIN_PCT_X1000="${EGIS0577_STAGE2_GRAIN_PCT_X1000-}" \
  EGIS0577_STAGE2_MIN_MINUTIAE="${EGIS0577_STAGE2_MIN_MINUTIAE-}" \
  EGIS0577_STAGE2_MAX_MINUTIAE="${EGIS0577_STAGE2_MAX_MINUTIAE-}" \
  EGIS0577_STAGE2_MIN_RIDGE_PIXELS="${EGIS0577_STAGE2_MIN_RIDGE_PIXELS-}" \
  EGIS0577_STAGE2_MIN_STRETCH_P5="${EGIS0577_STAGE2_MIN_STRETCH_P5-}" \
  EGIS0577_DISABLE_STRETCH="${EGIS0577_DISABLE_STRETCH-}" \
  EH577_CAPTURE_VERBOSE_STATUS="${EH577_CAPTURE_VERBOSE_STATUS-}" \
  bash -c '
    set -o pipefail
    "$1" "$2" "$3" 2>&1 |
      while IFS= read -r line; do
        case "$line" in
          EH577_CAPTURE\ finger-status\ *)
            ;;
          EH577_CAPTURE\ *)
            printf "%s\n" "$line"
            ;;
          *Stage-2\ reject:*)
            printf "%s\n" "$line" >> "$4"
            printf "EH577_CAPTURE reject %s\n" "${line#*Stage-2 reject: }"
            ;;
          *)
            printf "%s\n" "$line" >> "$4"
            ;;
        esac
      done
    exit ${PIPESTATUS[0]}
  ' _ "$HELPER" "$OUT" "$COUNT" "$LOG"
status=$?

if (( status != 0 )); then
  echo "Capture helper exited with status $status"
fi

sudo chown -R "$(id -u):$(id -g)" "$OUT" 2>/dev/null || true

echo ""
echo "Rendering PNGs + stats..."
have_convert=0; command -v convert >/dev/null 2>&1 && have_convert=1
for pgm in "$OUT"/capture-*.pgm; do
  [[ -e "$pgm" ]] || continue
  (( have_convert )) && convert "$pgm" -filter point -resize 300% "${pgm%.pgm}.png"
done

python3 - "$OUT" "$LOG" <<'PY'
import sys, glob, os, re
out = sys.argv[1]
log = sys.argv[2]
def load(p):
    d = open(p,'rb').read(); i = 2; v = []
    while len(v) < 3:
        while d[i] in b' \t\n\r': i += 1
        s = i
        while d[i] not in b' \t\n\r': i += 1
        v.append(int(d[s:i]))
    i += 1; w,h,_ = v; return w,h,d[i:i+w*h]

minutiae_list = []
ridges_list = []
if os.path.exists(log):
    with open(log, 'r', encoding='utf-8', errors='ignore') as f:
        for line in f:
            if '=> accept' in line:
                m1 = re.search(r'minutiae=(\d+)', line)
                m2 = re.search(r'ridge_pixels=(\d+)', line)
                if m1 and m2:
                    minutiae_list.append(int(m1.group(1)))
                    ridges_list.append(int(m2.group(1)))

print("\n  file                 dark<60  speckle  mean  minutiae  ridges   (speckle high = noisy)")
files = sorted(glob.glob(os.path.join(out,"capture-*.pgm")))
for idx, f in enumerate(files):
    w,h,px = load(f)
    dark = sum(1 for x in px if x < 60)
    spk = 0
    for y in range(1,h-1):
        for x in range(1,w-1):
            if px[y*w+x] < 60:
                lit = sum(1 for dy in(-1,0,1) for dx in(-1,0,1) if px[(y+dy)*w+(x+dx)] >= 120)
                if lit >= 6: spk += 1
    mi = str(minutiae_list[idx]) if idx < len(minutiae_list) else "?"
    ri = str(ridges_list[idx]) if idx < len(ridges_list) else "?"
    print(f"  {os.path.basename(f):20s} {dark:7d}  {spk:7d}  {sum(px)/len(px):4.0f}  {mi:>8}  {ri:>6}")
PY

echo ""
echo "=== DONE ==="
echo "PGMs + PNGs: $OUT"
echo "Eyeball the capture-NN.png files, then tell me which are clean vs noisy."
