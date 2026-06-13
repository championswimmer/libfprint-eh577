#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import List

ACCEPT_RE = re.compile(r"Stage-2 snapshot gate: .* minutiae=(\d+)/(\d+)\.\.(\d+) => accept")
MIN_LINE_RE = re.compile(r"^(?P<path>.+?) minutiae=(?P<count>\d+)$")
ROW_RE = re.compile(r"^\s*(\d+) \|(.*)\(min=(\d+)\)\s*$")
INT_RE = re.compile(r"-?\d+")


def run(cmd: List[str]) -> str:
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, check=True)
    return res.stdout


def parse_accept_counts(debug_log: Path) -> list[int]:
    counts = []
    for line in debug_log.read_text().splitlines():
        m = ACCEPT_RE.search(line)
        if m:
            counts.append(int(m.group(1)))
    return counts


def parse_minutiae_counts(text: str) -> dict[str, int]:
    counts: dict[str, int] = {}
    for line in text.splitlines():
        m = MIN_LINE_RE.match(line.strip())
        if m:
            counts[m.group("path")] = int(m.group("count"))
    return counts


def parse_matrix(text: str, expected_rows: int) -> list[list[int]]:
    matrix: list[list[int]] = []
    for line in text.splitlines():
        m = ROW_RE.match(line)
        if not m:
            continue
        nums = [int(x) for x in INT_RE.findall(m.group(2))]
        if len(nums) >= expected_rows:
            matrix.append(nums[:expected_rows])
    return matrix


def summarize_matrix(matrix: list[list[int]]) -> dict:
    off = []
    diag = []
    for i, row in enumerate(matrix):
        for j, score in enumerate(row):
            if i == j:
                diag.append(score)
            else:
                off.append(score)
    return {
        "diag": diag,
        "off_diag": off,
        "off_diag_max": max(off) if off else None,
        "off_diag_min": min(off) if off else None,
        "off_diag_avg": (sum(off) / len(off)) if off else None,
        "off_diag_nonzero": sum(1 for s in off if s != 0),
    }


def main() -> int:
    p = argparse.ArgumentParser(description="Offline EH577 PGM minutiae + bozorth workflow")
    p.add_argument("folder", help="Capture folder containing capture-*.pgm and optionally debug.log")
    p.add_argument("--build-dir", default="refs/libfprint/build/examples", help="Directory containing built helper binaries")
    p.add_argument("--threshold", type=int, default=9, help="Threshold to highlight in match matrices")
    p.add_argument("--json-out", help="Optional JSON report path")
    args = p.parse_args()

    folder = Path(args.folder)
    build_dir = Path(args.build_dir)
    minute_bin = build_dir / "eh577-pgm-minutiae"
    match_bin = build_dir / "eh577-pgm-match"
    relaxed_bin = build_dir / "eh577-pgm-match-relaxed"

    for tool in (minute_bin, match_bin, relaxed_bin):
        if not tool.exists():
            print(f"missing helper binary: {tool}", file=sys.stderr)
            return 2

    pgms = sorted(folder.glob("capture-*.pgm"))
    if not pgms:
        print(f"no capture-*.pgm files found in {folder}", file=sys.stderr)
        return 2

    print(f"Folder: {folder}")
    print(f"PGMs:   {len(pgms)}")

    debug_counts: list[int] = []
    debug_log = folder / "debug.log"
    if debug_log.exists():
        debug_counts = parse_accept_counts(debug_log)
        print(f"debug.log accepted counts: {debug_counts}")
    else:
        print("debug.log accepted counts: <missing>")

    pgm_args = [str(pgm) for pgm in pgms]

    minutiae_out = run([str(minute_bin), *pgm_args])
    minutiae_counts = parse_minutiae_counts(minutiae_out)

    print("\nOffline minutiae counts:")
    rows = []
    for idx, pgm in enumerate(pgms, start=1):
        offline = minutiae_counts.get(str(pgm))
        driver = debug_counts[idx - 1] if idx - 1 < len(debug_counts) else None
        match = (offline == driver) if driver is not None else None
        rows.append({
            "index": idx,
            "file": str(pgm),
            "driver_count": driver,
            "offline_count": offline,
            "counts_match": match,
        })
        print(f"  {idx:2d}. offline={offline!s:>2}  driver={str(driver) if driver is not None else '-':>2}  match={match}")

    standard_out = run([str(match_bin), *pgm_args, "--threshold", str(args.threshold)])
    relaxed_out = run([str(relaxed_bin), *pgm_args, "--threshold", str(args.threshold)])

    standard_matrix = parse_matrix(standard_out, len(pgms))
    relaxed_matrix = parse_matrix(relaxed_out, len(pgms))

    standard_summary = summarize_matrix(standard_matrix)
    relaxed_summary = summarize_matrix(relaxed_matrix)

    print("\nStandard Bozorth summary:")
    print(f"  off-diagonal max={standard_summary['off_diag_max']} avg={standard_summary['off_diag_avg']} nonzero={standard_summary['off_diag_nonzero']}")
    print("\nRelaxed Bozorth summary:")
    print(f"  off-diagonal max={relaxed_summary['off_diag_max']} avg={relaxed_summary['off_diag_avg']} nonzero={relaxed_summary['off_diag_nonzero']}")

    print("\n=== Standard matrix output ===\n")
    print(standard_out.rstrip())
    print("\n=== Relaxed matrix output ===\n")
    print(relaxed_out.rstrip())

    report = {
        "folder": str(folder),
        "pgms": [str(p) for p in pgms],
        "threshold": args.threshold,
        "debug_accept_counts": debug_counts,
        "minutiae_rows": rows,
        "standard_matrix": standard_matrix,
        "standard_summary": standard_summary,
        "relaxed_matrix": relaxed_matrix,
        "relaxed_summary": relaxed_summary,
        "raw_standard_output": standard_out,
        "raw_relaxed_output": relaxed_out,
    }

    if args.json_out:
        out = Path(args.json_out)
        out.write_text(json.dumps(report, indent=2))
        print(f"\nWrote JSON report: {out}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
