#!/usr/bin/env python3
import sys
import argparse
from pathlib import Path
from pgm_stats import read_pgm, calculate_stats

def center_of_mass(img, stats):
    if stats['non_zero'] == 0:
        return 0, 0
    
    cx = 0
    cy = 0
    w = img.width
    data = img.data
    
    total_weight = 0
    for y in range(img.height):
        for x in range(img.width):
            val = data[y * w + x]
            if val > 0:
                cx += x * val
                cy += y * val
                total_weight += val
                
    if total_weight == 0:
        return 0, 0
    return cx / total_weight, cy / total_weight

def main():
    p = argparse.ArgumentParser(description="Compare two PGM files")
    p.add_argument("enrolled", help="Enrolled PGM file")
    p.add_argument("identify", help="Identify PGM file")
    args = p.parse_args()

    try:
        img1 = read_pgm(Path(args.enrolled))
        img2 = read_pgm(Path(args.identify))
    except Exception as e:
        print(f"Error parsing PGM: {e}")
        sys.exit(1)

    st1 = calculate_stats(img1)
    st2 = calculate_stats(img2)

    print(f"Comparing:")
    print(f"  1: {args.enrolled}")
    print(f"  2: {args.identify}")
    print("-" * 40)

    # Dimension match
    dim_match = (st1['width'] == st2['width']) and (st1['height'] == st2['height'])
    print(f"Dimensions Match: {dim_match} ({st1['width']}x{st1['height']} vs {st2['width']}x{st2['height']})")

    # Occupancy
    area = st1['width'] * st1['height']
    occ1 = st1['non_zero'] / area if area > 0 else 0
    occ2 = st2['non_zero'] / area if area > 0 else 0
    occ_delta = occ2 - occ1
    print(f"Occupancy Ratio:  {occ1*100:.1f}% vs {occ2*100:.1f}% (delta: {occ_delta*100:+.1f}%)")

    # Center of Mass
    cm1_x, cm1_y = center_of_mass(img1, st1)
    cm2_x, cm2_y = center_of_mass(img2, st2)
    dx = cm2_x - cm1_x
    dy = cm2_y - cm1_y
    print(f"Center of Mass:   ({cm1_x:.1f}, {cm1_y:.1f}) vs ({cm2_x:.1f}, {cm2_y:.1f}) (delta: dx={dx:+.1f}, dy={dy:+.1f})")

    # Polarity / Inverted heuristic
    # A simple check: if one image's mean is very low and the other is very high, they might be inverted.
    # Also check background color by looking at the corner pixels (usually background).
    c1 = img1.data[0]
    c2 = img2.data[0]
    polarity_issue = False
    if (st1['mean'] > 128 and st2['mean'] < 128) or (st1['mean'] < 128 and st2['mean'] > 128):
        polarity_issue = True
    
    if abs(c1 - c2) > 128:
        polarity_issue = True

    print(f"Polarity Match:   {'Warning: possible inversion' if polarity_issue else 'OK'} (Mean: {st1['mean']:.1f} vs {st2['mean']:.1f}, Corner: {c1} vs {c2})")

if __name__ == "__main__":
    main()
