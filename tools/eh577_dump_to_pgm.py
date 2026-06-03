#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser(description="Convert an EH577 raw payload dump to a PGM image.")
    p.add_argument("input", help="Input raw payload file")
    p.add_argument("output", nargs="?", help="Output .pgm path (default: input + .pgm)")
    p.add_argument("--width", type=int, default=103, help="Image width (default: 103)")
    p.add_argument("--height", type=int, default=52, help="Image height (default: 52)")
    args = p.parse_args()

    inp = Path(args.input)
    out = Path(args.output) if args.output else inp.with_suffix(inp.suffix + ".pgm")
    data = inp.read_bytes()
    expected = args.width * args.height
    if len(data) != expected:
      raise SystemExit(f"size mismatch: got {len(data)} bytes, expected {expected} for {args.width}x{args.height}")

    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("wb") as f:
        f.write(f"P5\n{args.width} {args.height}\n255\n".encode())
        f.write(data)

    nonzero = sum(1 for b in data if b)
    print(f"wrote {out}")
    print(f"len={len(data)} nonzero={nonzero} zero={len(data)-nonzero} unique={len(set(data))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
