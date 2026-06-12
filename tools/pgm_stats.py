#!/usr/bin/env python3
import sys
import argparse
from pathlib import Path
from dataclasses import dataclass
from typing import List, Tuple, Optional

@dataclass
class PGMImage:
    width: int
    height: int
    max_val: int
    data: bytes

def read_pgm(path: Path) -> PGMImage:
    with path.open("rb") as f:
        # PGM header can be space-separated or newline-separated.
        # We need to read exactly "P5", then width, height, max_val, then data.
        
        # Read until we have 4 tokens (P5, width, height, maxval)
        tokens = []
        while len(tokens) < 4:
            line = f.readline()
            if not line:
                raise ValueError("Unexpected EOF reading PGM header")
            
            # strip comments
            comment_idx = line.find(b'#')
            if comment_idx != -1:
                line = line[:comment_idx]
                
            tokens.extend(line.split())
            
        if tokens[0] != b'P5':
            raise ValueError("Not a valid binary PGM (P5)")
            
        width = int(tokens[1])
        height = int(tokens[2])
        max_val = int(tokens[3])

        data = f.read()
        if len(data) != width * height:
            raise ValueError(f"Data size mismatch. Expected {width * height}, got {len(data)}")
        
        return PGMImage(width, height, max_val, data)

def calculate_stats(img: PGMImage) -> dict:
    data = img.data
    width = img.width
    height = img.height

    if not data:
        return {}

    min_val = min(data)
    max_val = max(data)
    mean_val = sum(data) / len(data)
    
    non_zero = sum(1 for p in data if p > 0)

    # Bounding box
    min_x, max_x = width, -1
    min_y, max_y = height, -1
    for y in range(height):
        for x in range(width):
            if data[y * width + x] > 0:
                if x < min_x: min_x = x
                if x > max_x: max_x = x
                if y < min_y: min_y = y
                if y > max_y: max_y = y

    if max_x < min_x:  # completely empty
        bbox = None
    else:
        bbox = (min_x, min_y, max_x, max_y)

    # Histogram (10 buckets)
    hist = [0] * 10
    if max_val > 0:
        for p in data:
            if p > 0:
                bucket = int((p / max_val) * 9.99)
                hist[bucket] += 1

    return {
        "width": width,
        "height": height,
        "min": min_val,
        "max": max_val,
        "mean": mean_val,
        "non_zero": non_zero,
        "bbox": bbox,
        "hist": hist
    }

def print_ascii_preview(img: PGMImage):
    chars = " .:-=+*#%@"
    w, h = img.width, img.height
    print("ASCII Preview:")
    
    # Contrast stretch
    min_val = min(img.data)
    max_val = max(img.data)
    if min_val == max_val:
        diff = 1
    else:
        diff = max_val - min_val

    for y in range(h):
        line = ""
        for x in range(w):
            val = img.data[y * w + x]
            norm = (val - min_val) / diff
            idx = int(norm * 9.99)
            line += chars[idx]
        print(line)

def main():
    p = argparse.ArgumentParser(description="PGM stats analyzer")
    p.add_argument("input", help="Input PGM file")
    p.add_argument("--preview", action="store_true", help="Print ASCII preview")
    args = p.parse_args()

    inp = Path(args.input)
    if not inp.exists():
        print(f"File not found: {inp}")
        sys.exit(1)

    try:
        img = read_pgm(inp)
    except Exception as e:
        print(f"Error parsing PGM: {e}")
        sys.exit(1)

    stats = calculate_stats(img)
    print(f"File:     {inp.name}")
    print(f"Size:     {stats['width']} x {stats['height']}")
    print(f"Min/Max:  {stats['min']} / {stats['max']}")
    print(f"Mean:     {stats['mean']:.2f}")
    
    total_pixels = stats['width'] * stats['height']
    pct_nonzero = (stats['non_zero'] / total_pixels) * 100 if total_pixels > 0 else 0
    print(f"Non-zero: {stats['non_zero']} ({pct_nonzero:.1f}%)")

    bbox = stats['bbox']
    if bbox:
        bw = bbox[2] - bbox[0] + 1
        bh = bbox[3] - bbox[1] + 1
        print(f"BBox:     x={bbox[0]}..{bbox[2]} y={bbox[1]}..{bbox[3]} (w={bw}, h={bh})")
    else:
        print("BBox:     empty")

    print(f"Hist:     {stats['hist']}")

    if args.preview:
        print()
        print_ascii_preview(img)

if __name__ == "__main__":
    main()
