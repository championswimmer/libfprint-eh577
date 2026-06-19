#!/usr/bin/env python3
"""Reproduce the driver's Stage-2 processing pipeline on raw EH577 frame dumps.

Pipeline (matches egis0577.c save_img + stage2_snapshot_quality_ok):
  1. Background subtraction: pixel = (raw > bg+2) ? raw-bg : 0  (72×52 padded buf)
  2. Bilinear resize 72×52 → 144×104  (pixman BILINEAR, factor=2)
  3. Invert: 255 - pixel  (COLORS_INVERTED normalisation)
  4. Stretch5: map p5→20, p99→245 in-place
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required: pip install Pillow")

# ---- constants (must match egis0577.h) ---------------------------------
SENSOR_STRIDE_X   = 103
SENSOR_STRIDE_Y   = 52
SENSOR_ACTIVE_W   = 70
IMGWIDTH          = SENSOR_ACTIVE_W       # 70
IMGHEIGHT         = SENSOR_STRIDE_Y       # 52
PADDED_IMGWIDTH   = ((IMGWIDTH + 3) // 4) * 4  # 72
RESIZE            = 2
FINAL_W           = PADDED_IMGWIDTH * RESIZE    # 144
FINAL_H           = IMGHEIGHT * RESIZE          # 104
STRETCH_LO_PCT    = 5
STRETCH_HI_PCT    = 99
STRETCH_OUT_LO    = 20
STRETCH_OUT_HI    = 245
FRAME_SIZE        = SENSOR_STRIDE_X * SENSOR_STRIDE_Y  # 5356


def load_raw(path: Path) -> bytes:
    data = path.read_bytes()
    if len(data) != FRAME_SIZE:
        raise ValueError(f"{path}: expected {FRAME_SIZE} bytes, got {len(data)}")
    return data


def bg_subtract(raw: bytes, bg: bytes) -> bytearray:
    """Build the 72×52 padded image with background subtraction."""
    buf = bytearray(PADDED_IMGWIDTH * IMGHEIGHT)  # zero-initialised
    for y in range(SENSOR_STRIDE_Y):
        for x in range(SENSOR_ACTIVE_W):
            val = raw[y * SENSOR_STRIDE_X + x]
            b   = bg[y * SENSOR_STRIDE_X + x] if bg else 0
            buf[y * PADDED_IMGWIDTH + x] = (val - b) if val > b + 2 else 0
    return buf


def percentile(data: bytearray, pct: int) -> int:
    """Histogram-based percentile matching the C implementation exactly."""
    hist = [0] * 256
    total = len(data)
    for v in data:
        hist[v] += 1
    target = (total - 1) * pct // 100
    cumulative = 0
    for i, h in enumerate(hist):
        cumulative += h
        if cumulative > target:
            return i
    return 255


def stretch5(pixels: bytearray) -> bytearray:
    lo = percentile(pixels, STRETCH_LO_PCT)
    hi = percentile(pixels, STRETCH_HI_PCT)
    if hi <= lo:
        return pixels  # flat histogram, skip
    out_range = STRETCH_OUT_HI - STRETCH_OUT_LO
    in_range  = hi - lo
    result = bytearray(len(pixels))
    for i, v in enumerate(pixels):
        if v <= lo:
            s = STRETCH_OUT_LO
        elif v >= hi:
            s = STRETCH_OUT_HI
        else:
            s = STRETCH_OUT_LO + ((v - lo) * out_range + in_range // 2) // in_range
        result[i] = max(0, min(255, s))
    return result


def process_frame(raw: bytes, bg: bytes | None) -> bytearray:
    # 1. Background subtraction → 72×52
    buf = bg_subtract(raw, bg)

    # 2. Bilinear resize 72×52 → 144×104
    img = Image.frombuffer("L", (PADDED_IMGWIDTH, IMGHEIGHT), bytes(buf), "raw", "L", 0, 1)
    img = img.resize((FINAL_W, FINAL_H), Image.BILINEAR)
    pixels = bytearray(img.tobytes())

    # 3. Invert (COLORS_INVERTED normalisation)
    pixels = bytearray(255 - v for v in pixels)

    # 4. Stretch5
    pixels = stretch5(pixels)

    return pixels


def write_pgm(path: Path, pixels: bytearray, w: int, h: int) -> None:
    header = f"P5\n{w} {h}\n255\n".encode()
    path.write_bytes(header + bytes(pixels))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("frames", nargs="+", metavar="FRAME.bin",
                    help="Raw 5356-byte EH577 frame dumps to process")
    ap.add_argument("--bg", metavar="BG.bin",
                    help="Background (no-finger) frame for subtraction (default: zero)")
    ap.add_argument("--outdir", metavar="DIR",
                    help="Output directory (default: same dir as each frame)")
    ap.add_argument("--suffix", default="-processed",
                    help="Suffix appended before .pgm (default: -processed)")
    args = ap.parse_args()

    bg: bytes | None = None
    if args.bg:
        bg = load_raw(Path(args.bg))

    for frame_path_str in args.frames:
        frame_path = Path(frame_path_str)
        raw = load_raw(frame_path)
        pixels = process_frame(raw, bg)

        stem = frame_path.stem          # e.g. "0144-post-init-nonzero-3017"
        # extract leading 4-digit index for a cleaner output name
        idx = stem.split("-")[0]
        out_name = f"stage2-{idx}{args.suffix}.pgm"

        if args.outdir:
            out_path = Path(args.outdir) / out_name
        else:
            # place alongside the session PGMs (parent of raw-enroll/)
            out_path = frame_path.parent.parent / out_name

        write_pgm(out_path, pixels, FINAL_W, FINAL_H)
        print(f"Written {out_path}  ({FINAL_W}×{FINAL_H})")


if __name__ == "__main__":
    main()
