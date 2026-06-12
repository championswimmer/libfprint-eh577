#!/usr/bin/env python3
"""Lightweight PE/DLL string and integer-pair scanner.

Useful for reverse-engineering Windows fingerprint drivers without external
libraries. It:
- parses PE sections
- extracts ASCII and UTF-16LE strings
- filters strings by keyword
- scans for candidate integer pairs (u16/u32) inside the file/sections
"""

from __future__ import annotations

import argparse
import re
import struct
from pathlib import Path
from typing import Iterable


ASCII_RE = re.compile(rb"[ -~]{4,}")
UTF16_RE = re.compile(rb"(?:[ -~]\x00){4,}")


def parse_pe_sections(data: bytes):
    if data[:2] != b"MZ":
        raise ValueError("Not an MZ/PE file")
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError("Missing PE signature")

    coff_off = pe_off + 4
    machine, num_sections, _time, _ptr_sym, _num_sym, size_opt, _chars = struct.unpack_from(
        "<HHIIIHH", data, coff_off
    )
    sec_off = coff_off + 20 + size_opt
    sections = []
    for i in range(num_sections):
        off = sec_off + i * 40
        raw_name = data[off:off + 8]
        name = raw_name.split(b"\0", 1)[0].decode("ascii", errors="replace")
        virtual_size, virtual_address, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off + 8)
        sections.append(
            {
                "name": name,
                "vaddr": virtual_address,
                "vsize": virtual_size,
                "raw_ptr": raw_ptr,
                "raw_size": raw_size,
            }
        )
    return {"machine": machine, "num_sections": num_sections, "sections": sections}


def section_for_offset(sections, offset: int) -> str:
    for sec in sections:
        start = sec["raw_ptr"]
        end = start + sec["raw_size"]
        if start <= offset < end:
            return sec["name"]
    return "<none>"


def iter_ascii_strings(data: bytes) -> Iterable[tuple[int, str]]:
    for m in ASCII_RE.finditer(data):
        yield m.start(), m.group().decode("ascii", errors="replace")


def iter_utf16le_strings(data: bytes) -> Iterable[tuple[int, str]]:
    for m in UTF16_RE.finditer(data):
        yield m.start(), m.group().decode("utf-16le", errors="replace")


def search_pairs(data: bytes, pairs: list[tuple[int, int]], width_bits: int):
    fmt = "<HH" if width_bits == 16 else "<II"
    size = 4 if width_bits == 16 else 8
    patterns = {pair: struct.pack(fmt, *pair) for pair in pairs}
    for pair, pat in patterns.items():
        start = 0
        while True:
            idx = data.find(pat, start)
            if idx < 0:
                break
            yield pair, idx, data[max(0, idx - 32): idx + size + 32]
            start = idx + 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+", help="PE/DLL files to scan")
    ap.add_argument("--keyword", action="append", default=[], help="case-insensitive string keyword filter")
    ap.add_argument(
        "--pair",
        action="append",
        default=[],
        help="candidate int pair A,B to search as little-endian u16/u32; may be repeated",
    )
    args = ap.parse_args()

    keywords = [k.lower() for k in args.keyword]
    pairs = []
    for item in args.pair:
        a, b = item.split(",", 1)
        pairs.append((int(a, 0), int(b, 0)))

    for file_name in args.files:
        path = Path(file_name)
        data = path.read_bytes()
        pe = parse_pe_sections(data)
        print(f"==== {path}")
        print(f"sections={pe['num_sections']}")
        for sec in pe["sections"]:
            print(
                f"  {sec['name']:8s} raw=0x{sec['raw_ptr']:x}-0x{sec['raw_ptr'] + sec['raw_size']:x} "
                f"vaddr=0x{sec['vaddr']:x} size=0x{sec['raw_size']:x}"
            )

        if keywords:
            print("-- ascii/unicode strings matching keywords --")
            seen = set()
            for kind, iterator in (("ascii", iter_ascii_strings(data)), ("utf16", iter_utf16le_strings(data))):
                for off, s in iterator:
                    sl = s.lower()
                    if any(k in sl for k in keywords):
                        key = (off, s)
                        if key in seen:
                            continue
                        seen.add(key)
                        print(f"[{kind}] off=0x{off:x} sec={section_for_offset(pe['sections'], off):8s} {s}")

        if pairs:
            print("-- candidate integer pairs --")
            for bits in (16, 32):
                for pair, off, blob in search_pairs(data, pairs, bits):
                    hex_blob = blob.hex()
                    print(
                        f"u{bits} pair={pair} off=0x{off:x} sec={section_for_offset(pe['sections'], off):8s} "
                        f"context={hex_blob}"
                    )


if __name__ == "__main__":
    main()
