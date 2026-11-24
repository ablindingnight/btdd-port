#!/usr/bin/env python3
"""Compare two RAM dump files (btdd_headless --dump-ram, tools/fceux/ramdump.lua).

Each file is a sequence of records: uint32 LE frame number + 2048 bytes of RAM. Frames present in both
files are compared in order; the first differing frame and its differing addresses are reported.

  python tools/ramdiff.py reference.bin candidate.bin [--names ram.inc] [--max-addrs 32] [--ignore 0x100-0x1FF]
"""
import argparse
import re
import struct
import sys

REC = 4 + 2048


def records(path):
    with open(path, "rb") as f:
        while True:
            hdr = f.read(4)
            if len(hdr) < 4:
                return
            ram = f.read(2048)
            if len(ram) < 2048:
                return
            yield struct.unpack("<I", hdr)[0], ram


def load_names(path):
    names = {}
    if not path:
        return names
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = re.match(r"\s*([A-Za-z_]\w*)\s*=\s*\$([0-9A-Fa-f]{1,4})\b", line)
                if m:
                    addr = int(m.group(2), 16)
                    if addr < 0x800:
                        names.setdefault(addr, m.group(1))
    except OSError:
        pass
    return names


def parse_ranges(specs):
    out = []
    for s in specs or []:
        lo, _, hi = s.partition("-")
        lo = int(lo, 0)
        out.append((lo, int(hi, 0) if hi else lo))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--names", help="ca65 .inc with 'name = $addr' lines (e.g. btdd-decomp src/ram.inc)")
    ap.add_argument("--max-addrs", type=int, default=32)
    ap.add_argument("--ignore", action="append", help="address range to skip, e.g. 0x100-0x1FF")
    ap.add_argument("--all", action="store_true", help="keep going and summarise every differing frame")
    args = ap.parse_args()

    names = load_names(args.names)
    ignore = parse_ranges(args.ignore)
    mask = [not any(lo <= a <= hi for lo, hi in ignore) for a in range(2048)]

    cand = {}
    for fr, ram in records(args.candidate):
        cand[fr] = ram
    compared = 0
    first = None
    bad_frames = 0
    last_frame = None
    for fr, ref in records(args.reference):
        other = cand.get(fr)
        if other is None:
            continue
        compared += 1
        last_frame = fr
        diffs = [a for a in range(2048) if mask[a] and ref[a] != other[a]]
        if diffs:
            bad_frames += 1
            if first is None:
                first = (fr, diffs, ref, other)
                if not args.all:
                    break

    if compared == 0:
        print("no common frames")
        return 2
    if first is None:
        print(f"identical: {compared} common frames compared, last frame {last_frame}")
        return 0
    fr, diffs, ref, other = first
    print(f"first difference at frame {fr} ({len(diffs)} bytes differ); "
          f"{compared} frames compared" + (f", {bad_frames} differ" if args.all else ""))
    print(" addr   ref  cand  name")
    for a in diffs[: args.max_addrs]:
        print(f" ${a:04X}  {ref[a]:02X}   {other[a]:02X}   {names.get(a, '')}")
    if len(diffs) > args.max_addrs:
        print(f" ... {len(diffs) - args.max_addrs} more")
    return 1


if __name__ == "__main__":
    sys.exit(main())
