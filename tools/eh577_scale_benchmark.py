#!/usr/bin/env python3
"""Offline EH577 scaling benchmark for captured PGM images.

Given a capture directory containing capture-*.pgm files, this script:
  1. generates nearest-neighbor 2x/3x scaled variants,
  2. runs eh577-pgm-minutiae on each image,
  3. runs eh577-pgm-match across each scale group,
  4. writes CSV/Markdown results into a subdirectory inside the capture folder.

The goal is to compare the current libfprint/NBIS pipeline on:
  - native images
  - 2x digital upscales
  - 3x digital upscales

This does not claim that more minutiae are better; noisy transforms can inflate
counts. Use the pairwise score outputs together with the minutiae counts.
"""

from __future__ import annotations

import argparse
import csv
import itertools
import re
import statistics
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


MINUTIAE_RE = re.compile(r"minutiae=(\d+)")
MINUTIAE_ERR_RE = re.compile(r"minutiae=ERR\((.*?)\)")
ROW_RE = re.compile(r"^\s*\d+\s*\|")
BRACKET_VALUE_RE = re.compile(r"\[\s*(-?\d+)\]")
PLAIN_INT_RE = re.compile(r"-?\d+")
MIN_RE = re.compile(r"\(min=(\d+)\)")


@dataclass
class PGM:
    width: int
    height: int
    maxval: int
    pixels: bytes


@dataclass
class MinutiaeRow:
    scale: int
    file: str
    path: str
    width: int
    height: int
    minutiae: int


@dataclass
class PairRow:
    scale: int
    file_a: str
    file_b: str
    path_a: str
    path_b: str
    minutiae_a: int
    minutiae_b: int
    score: int


def read_pgm(path: Path) -> PGM:
    data = path.read_bytes()
    if not data.startswith(b"P5"):
        raise ValueError(f"{path}: only binary P5 PGM is supported")

    i = 2
    toks: list[int] = []
    while len(toks) < 3:
        while i < len(data) and data[i] in b" \t\r\n":
            i += 1
        if i >= len(data):
            raise ValueError(f"{path}: unexpected EOF in header")
        if data[i] == 35:  # '#'
            while i < len(data) and data[i] not in b"\r\n":
                i += 1
            continue
        start = i
        while i < len(data) and data[i] not in b" \t\r\n":
            i += 1
        toks.append(int(data[start:i]))

    while i < len(data) and data[i] in b" \t\r\n":
        i += 1

    width, height, maxval = toks
    pixels = data[i:i + width * height]
    if len(pixels) != width * height:
        raise ValueError(
            f"{path}: pixel data size mismatch, expected {width * height}, got {len(pixels)}"
        )
    return PGM(width, height, maxval, pixels)


def write_pgm(path: Path, img: PGM) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(f"P5\n{img.width} {img.height}\n{img.maxval}\n".encode() + img.pixels)


def upscale_nearest(img: PGM, scale: int) -> PGM:
    if scale == 1:
        return img
    if scale < 1:
        raise ValueError("scale must be >= 1")

    out_w = img.width * scale
    out_h = img.height * scale
    src = img.pixels
    out = bytearray(out_w * out_h)

    for y in range(img.height):
        src_row = y * img.width
        for sy in range(scale):
            dst_row = (y * scale + sy) * out_w
            for x in range(img.width):
                val = src[src_row + x]
                start = dst_row + x * scale
                out[start:start + scale] = bytes([val]) * scale

    return PGM(out_w, out_h, img.maxval, bytes(out))


def run(cmd: list[str]) -> str:
    proc = subprocess.run(cmd, capture_output=True, text=True)
    output = proc.stdout + proc.stderr
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(cmd)}\n{output}"
        )
    return output


def parse_minutiae(output: str) -> int:
    m = MINUTIAE_RE.search(output)
    if m:
        return int(m.group(1))

    err = MINUTIAE_ERR_RE.search(output)
    if err:
        return 0

    raise ValueError(f"could not parse minutiae count from output:\n{output}")


def parse_matrix(output: str, n: int) -> tuple[list[list[int]], list[int]]:
    rows: list[str] = []
    seen_matrix = False
    for line in output.splitlines():
        if "Bozorth3 score matrix" in line:
            seen_matrix = True
            continue
        if seen_matrix and ROW_RE.match(line):
            rows.append(line)
            if len(rows) == n:
                break

    if len(rows) != n:
        raise ValueError(f"could not parse {n} score-matrix rows from output:\n{output}")

    matrix: list[list[int]] = []
    mins: list[int] = []
    for line in rows:
        values = [int(v) for v in BRACKET_VALUE_RE.findall(line)]
        if not values:
            segment = line.split("|", 1)[1].split("(min=", 1)[0]
            values = [int(v) for v in PLAIN_INT_RE.findall(segment)]
        if len(values) != n:
            raise ValueError(f"unexpected row format while parsing match matrix:\n{line}")
        m = MIN_RE.search(line)
        mins.append(int(m.group(1)) if m else -1)
        matrix.append(values)

    return matrix, mins


def capture_pgms(capture_dir: Path) -> list[Path]:
    files = sorted(capture_dir.glob("capture-*.pgm"))
    if not files:
        raise SystemExit(f"no capture-*.pgm files found in {capture_dir}")
    return files


def ensure_scaled_images(files: Iterable[Path], scale: int, out_dir: Path) -> list[Path]:
    out_dir.mkdir(parents=True, exist_ok=True)
    out_files: list[Path] = []
    for src in files:
        dst = out_dir / src.name
        if not dst.exists():
            img = read_pgm(src)
            write_pgm(dst, upscale_nearest(img, scale))
        out_files.append(dst)
    return out_files


def benchmark_scale(
    scale: int,
    files: list[Path],
    minutiae_tool: Path,
    match_tool: Path,
    threshold: int,
) -> tuple[list[MinutiaeRow], list[PairRow], list[list[int]]]:
    minutiae_rows: list[MinutiaeRow] = []
    minutiae_by_name: dict[str, int] = {}

    for path in files:
        img = read_pgm(path)
        out = run([str(minutiae_tool), str(path)])
        count = parse_minutiae(out)
        minutiae_rows.append(
            MinutiaeRow(
                scale=scale,
                file=path.name,
                path=str(path),
                width=img.width,
                height=img.height,
                minutiae=count,
            )
        )
        minutiae_by_name[path.name] = count

    match_out = run([str(match_tool), *map(str, files), "--threshold", str(threshold)])
    matrix, _ = parse_matrix(match_out, len(files))

    pair_rows: list[PairRow] = []
    for i, j in itertools.combinations(range(len(files)), 2):
        a = files[i]
        b = files[j]
        pair_rows.append(
            PairRow(
                scale=scale,
                file_a=a.name,
                file_b=b.name,
                path_a=str(a),
                path_b=str(b),
                minutiae_a=minutiae_by_name[a.name],
                minutiae_b=minutiae_by_name[b.name],
                score=matrix[i][j],
            )
        )

    return minutiae_rows, pair_rows, matrix


def write_minutiae_csv(path: Path, rows: list[MinutiaeRow]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["scale", "file", "path", "width", "height", "minutiae"])
        for r in rows:
            w.writerow([r.scale, r.file, r.path, r.width, r.height, r.minutiae])


def write_pairs_csv(path: Path, rows: list[PairRow]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow([
            "scale",
            "file_a",
            "file_b",
            "path_a",
            "path_b",
            "minutiae_a",
            "minutiae_b",
            "score",
        ])
        for r in rows:
            w.writerow([
                r.scale,
                r.file_a,
                r.file_b,
                r.path_a,
                r.path_b,
                r.minutiae_a,
                r.minutiae_b,
                r.score,
            ])


def write_matrix_csv(path: Path, files: list[Path], matrix: list[list[int]]) -> None:
    with path.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["file", *[p.name for p in files]])
        for p, row in zip(files, matrix):
            w.writerow([p.name, *row])


def summarize_scale(min_rows: list[MinutiaeRow], pair_rows: list[PairRow]) -> dict[str, float | int]:
    mins = [r.minutiae for r in min_rows]
    scores = [r.score for r in pair_rows]
    return {
        "images": len(min_rows),
        "pairs": len(pair_rows),
        "minutiae_min": min(mins),
        "minutiae_max": max(mins),
        "minutiae_mean": statistics.mean(mins),
        "minutiae_ge_10": sum(1 for m in mins if m >= 10),
        "score_min": min(scores) if scores else 0,
        "score_max": max(scores) if scores else 0,
        "score_mean": statistics.mean(scores) if scores else 0.0,
        "score_gt_0": sum(1 for s in scores if s > 0),
    }


def write_summary_md(
    path: Path,
    capture_dir: Path,
    result_dir: Path,
    scales: list[int],
    by_scale: dict[int, tuple[list[MinutiaeRow], list[PairRow], list[list[int]], list[Path]]],
    threshold: int,
) -> None:
    lines: list[str] = []
    lines.append("# EH577 scale benchmark\n")
    lines.append(f"- Capture dir: `{capture_dir}`")
    lines.append(f"- Result dir: `{result_dir}`")
    lines.append(f"- Scales: `{', '.join(map(str, scales))}`")
    lines.append(f"- Match threshold used for matrix printing: `{threshold}`")
    lines.append("")
    lines.append("## Per-scale summary\n")
    lines.append("| Scale | Images | Pairs | Minutiae min/max/mean | Images with >=10 minutiae | Score min/max/mean | Non-zero pair scores |")
    lines.append("|---:|---:|---:|---|---:|---|---:|")

    for scale in scales:
        min_rows, pair_rows, _matrix, _files = by_scale[scale]
        s = summarize_scale(min_rows, pair_rows)
        lines.append(
            f"| {scale}x | {s['images']} | {s['pairs']} | "
            f"{s['minutiae_min']}/{s['minutiae_max']}/{s['minutiae_mean']:.2f} | "
            f"{s['minutiae_ge_10']} | "
            f"{s['score_min']}/{s['score_max']}/{s['score_mean']:.2f} | {s['score_gt_0']} |"
        )

    lines.append("")
    lines.append("## Top pair scores by scale\n")
    for scale in scales:
        _min_rows, pair_rows, _matrix, _files = by_scale[scale]
        lines.append(f"### {scale}x\n")
        top = sorted(pair_rows, key=lambda r: (-r.score, r.file_a, r.file_b))[:10]
        if not top:
            lines.append("No pairs.\n")
            continue
        lines.append("| A | B | Minutiae A | Minutiae B | Score |")
        lines.append("|---|---|---:|---:|---:|")
        for r in top:
            lines.append(
                f"| `{r.file_a}` | `{r.file_b}` | {r.minutiae_a} | {r.minutiae_b} | {r.score} |"
            )
        lines.append("")

    lines.append("## Files\n")
    lines.append("- `minutiae.csv` — one row per image per scale")
    lines.append("- `pairs.csv` — one row per unordered image pair per scale")
    for scale in scales:
        lines.append(f"- `matrix-{scale}x.csv` — full NxN score matrix for {scale}x")
    if any(scale > 1 for scale in scales):
        lines.append("- `scaled/` — generated upscaled PGM images")
    lines.append("")
    lines.append("> Note: higher minutiae counts are not automatically better; noisy images can inflate counts.")
    path.write_text("\n".join(lines) + "\n")


def main() -> int:
    p = argparse.ArgumentParser(description="Run an offline native-vs-scaled EH577 benchmark")
    p.add_argument("capture_dir", help="Directory containing capture-*.pgm files")
    p.add_argument(
        "--scales",
        default="1,2,3",
        help="Comma-separated integer scales to test (default: 1,2,3)",
    )
    p.add_argument(
        "--result-dir",
        help="Output directory (default: <capture_dir>/scale-benchmark)",
    )
    p.add_argument(
        "--minutiae-tool",
        default="refs/libfprint/build/examples/eh577-pgm-minutiae",
        help="Path to eh577-pgm-minutiae",
    )
    p.add_argument(
        "--match-tool",
        default="refs/libfprint/build/examples/eh577-pgm-match",
        help="Path to eh577-pgm-match",
    )
    p.add_argument(
        "--threshold",
        type=int,
        default=0,
        help="Threshold passed to eh577-pgm-match when printing matrices (default: 0 for raw-score visibility)",
    )
    args = p.parse_args()

    capture_dir = Path(args.capture_dir).resolve()
    result_dir = Path(args.result_dir).resolve() if args.result_dir else capture_dir / "scale-benchmark"
    result_dir.mkdir(parents=True, exist_ok=True)

    minutiae_tool = Path(args.minutiae_tool).resolve()
    match_tool = Path(args.match_tool).resolve()
    if not minutiae_tool.exists():
        raise SystemExit(f"missing minutiae tool: {minutiae_tool}")
    if not match_tool.exists():
        raise SystemExit(f"missing match tool: {match_tool}")

    scales = [int(part.strip()) for part in args.scales.split(",") if part.strip()]
    scales = sorted(dict.fromkeys(scales))
    if any(s < 1 for s in scales):
        raise SystemExit("all scales must be >= 1")

    originals = capture_pgms(capture_dir)

    all_minutiae: list[MinutiaeRow] = []
    all_pairs: list[PairRow] = []
    by_scale: dict[int, tuple[list[MinutiaeRow], list[PairRow], list[list[int]], list[Path]]] = {}

    for scale in scales:
        if scale == 1:
            files = originals
        else:
            files = ensure_scaled_images(originals, scale, result_dir / "scaled" / f"{scale}x")

        min_rows, pair_rows, matrix = benchmark_scale(
            scale=scale,
            files=files,
            minutiae_tool=minutiae_tool,
            match_tool=match_tool,
            threshold=args.threshold,
        )
        by_scale[scale] = (min_rows, pair_rows, matrix, files)
        all_minutiae.extend(min_rows)
        all_pairs.extend(pair_rows)
        write_matrix_csv(result_dir / f"matrix-{scale}x.csv", files, matrix)

    write_minutiae_csv(result_dir / "minutiae.csv", all_minutiae)
    write_pairs_csv(result_dir / "pairs.csv", all_pairs)
    write_summary_md(
        result_dir / "summary.md",
        capture_dir=capture_dir,
        result_dir=result_dir,
        scales=scales,
        by_scale=by_scale,
        threshold=args.threshold,
    )

    print(f"wrote results to {result_dir}")
    print(f"- {result_dir / 'minutiae.csv'}")
    print(f"- {result_dir / 'pairs.csv'}")
    print(f"- {result_dir / 'summary.md'}")
    for scale in scales:
        print(f"- {result_dir / f'matrix-{scale}x.csv'}")
    if any(scale > 1 for scale in scales):
        print(f"- {result_dir / 'scaled'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
