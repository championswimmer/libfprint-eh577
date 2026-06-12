#!/usr/bin/env python3
"""Experiment with EH577 PGM contrast/ridge-visibility transforms.

This is an offline calibration helper. It reads capture-*.pgm files, writes enhanced
variants, and optionally runs refs/libfprint/build/examples/eh577-pgm-minutiae to
show whether a transform increases NBIS minutiae.

The goal is not to declare higher minutiae automatically better: noisy enhancement can
manufacture false minutiae. Use this together with visual inspection and Bozorth
cross-press scores.
"""

from __future__ import annotations

import argparse
import glob
import math
import os
import subprocess
from pathlib import Path


def load_pgm(path: str):
    data = Path(path).read_bytes()
    if data[:2] != b"P5":
        raise ValueError(f"{path}: only binary P5 PGM is supported")
    i = 2
    toks = []
    while len(toks) < 3:
        while data[i] in b" \t\r\n":
            i += 1
        if data[i] == 35:  # '#'
            while data[i] not in b"\r\n":
                i += 1
            continue
        start = i
        while data[i] not in b" \t\r\n":
            i += 1
        toks.append(int(data[start:i]))
    while data[i] in b" \t\r\n":
        i += 1
    w, h, maxv = toks
    if maxv != 255:
        raise ValueError(f"{path}: expected maxval 255, got {maxv}")
    pixels = list(data[i:i + w * h])
    if len(pixels) != w * h:
        raise ValueError(f"{path}: truncated pixel data")
    return w, h, pixels


def save_pgm(path: str, w: int, h: int, pixels):
    out = bytes(max(0, min(255, int(round(v)))) for v in pixels)
    Path(path).write_bytes(f"P5\n{w} {h}\n255\n".encode() + out)


def percentile(values, pct: float):
    vals = sorted(values)
    idx = int((len(vals) - 1) * pct / 100.0)
    return vals[max(0, min(len(vals) - 1, idx))]


def stretch(pixels, lo_pct=1, hi_pct=99, out_lo=20, out_hi=245):
    lo = percentile(pixels, lo_pct)
    hi = percentile(pixels, hi_pct)
    if hi <= lo:
        return list(pixels)
    scale = (out_hi - out_lo) / (hi - lo)
    return [out_lo + (p - lo) * scale for p in pixels]


def gamma(pixels, g: float):
    return [255.0 * ((p / 255.0) ** g) for p in pixels]


def blur_box(pixels, w, h, radius=1):
    out = []
    for y in range(h):
        for x in range(w):
            total = 0
            count = 0
            for yy in range(max(0, y - radius), min(h, y + radius + 1)):
                row = yy * w
                for xx in range(max(0, x - radius), min(w, x + radius + 1)):
                    total += pixels[row + xx]
                    count += 1
            out.append(total / count)
    return out


def unsharp(pixels, w, h, amount=0.4, radius=1):
    blurred = blur_box(pixels, w, h, radius)
    return [p + amount * (p - b) for p, b in zip(pixels, blurred)]


def median3(pixels, w, h):
    out = []
    for y in range(h):
        for x in range(w):
            vals = []
            for yy in range(max(0, y - 1), min(h, y + 2)):
                row = yy * w
                for xx in range(max(0, x - 1), min(w, x + 2)):
                    vals.append(pixels[row + xx])
            vals.sort()
            out.append(vals[len(vals) // 2])
    return out


def localnorm(pixels, w, h, radius=8, gain=36, min_sd=6):
    """Local z-score contrast normalization.

    Dark ridges remain dark. Low-contrast neighborhoods are left mostly unchanged
    to avoid amplifying flat background noise.
    """
    out = []
    for y in range(h):
        for x in range(w):
            vals = []
            for yy in range(max(0, y - radius), min(h, y + radius + 1)):
                row = yy * w
                vals.extend(pixels[row + max(0, x - radius):row + min(w, x + radius + 1)])
            mean = sum(vals) / len(vals)
            var = sum((v - mean) ** 2 for v in vals) / len(vals)
            sd = math.sqrt(var)
            if sd < min_sd:
                out.append(pixels[y * w + x])
            else:
                out.append(128.0 + gain * ((pixels[y * w + x] - mean) / sd))
    return out


def grain_pct(pixels, w, h):
    noisy = 0
    total = 0
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            vals = [pixels[(y + dy) * w + x + dx] for dy in (-1, 0, 1) for dx in (-1, 0, 1)]
            vals.sort()
            if abs(pixels[y * w + x] - vals[4]) > 25:
                noisy += 1
            total += 1
    return 100.0 * noisy / total if total else 999.0


def minutiae_count(tool: str, path: str):
    if not tool or not os.path.exists(tool):
        return None
    out = subprocess.check_output([tool, path], text=True, stderr=subprocess.DEVNULL).strip()
    return int(out.rsplit("=", 1)[1])


def variants(pixels, w, h):
    yield "orig", pixels
    yield "gamma12", gamma(pixels, 1.2)
    yield "stretch1", stretch(pixels, 1, 99)
    yield "stretch5", stretch(pixels, 5, 99)
    yield "med_stretch", stretch(median3(pixels, w, h), 1, 99)
    ln = localnorm(pixels, w, h, radius=8, gain=36)
    yield "local36", ln
    yield "local36_us", unsharp(ln, w, h, amount=0.4, radius=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input", help="PGM file or directory containing capture-*.pgm")
    ap.add_argument("--out", help="output directory", default=None)
    ap.add_argument("--minutiae-tool", default="refs/libfprint/build/examples/eh577-pgm-minutiae")
    args = ap.parse_args()

    inputs = [args.input]
    if os.path.isdir(args.input):
        inputs = sorted(glob.glob(os.path.join(args.input, "capture-*.pgm")))
    outdir = args.out or os.path.join(os.path.dirname(inputs[0]), "enhance-experiment")
    os.makedirs(outdir, exist_ok=True)

    rows = []
    for src in inputs:
        w, h, pixels = load_pgm(src)
        stem = Path(src).stem
        for name, enhanced in variants(pixels, w, h):
            dst = os.path.join(outdir, f"{stem}-{name}.pgm")
            save_pgm(dst, w, h, enhanced)
            w2, h2, px2 = load_pgm(dst)
            rows.append((stem, name, minutiae_count(args.minutiae_tool, dst),
                         grain_pct(px2, w2, h2), sum(1 for p in px2 if p < 180), dst))

    print(f"wrote variants to {outdir}")
    print("capture variant minutiae grain% ridge_pixels path")
    for cap, name, mins, grain, ridge, dst in rows:
        mins_s = "NA" if mins is None else str(mins)
        print(f"{cap:12s} {name:12s} {mins_s:>8s} {grain:7.3f} {ridge:12d} {dst}")


if __name__ == "__main__":
    main()
