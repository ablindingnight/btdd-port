#!/usr/bin/env python3
"""Compare palette-index screen dumps: FCEUX (screendump.lua: fceux_idx_N.bin) vs btdd_shot (idx_N.bin).

  python tools/shotdiff.py FCEUX_DIR OUR_DIR [frames...]
Prints, per frame, how many of the 61,440 pixels differ and where the first rows differ.
"""
import os
import re
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    ref_dir, our_dir = sys.argv[1], sys.argv[2]
    frames = [int(x) for x in sys.argv[3:]]
    if not frames:
        frames = sorted(int(m.group(1)) for f in os.listdir(ref_dir)
                        for m in [re.match(r"fceux_idx_(\d+)\.bin$", f)] if m)
    worst = 0
    for n in frames:
        try:
            a = open(os.path.join(ref_dir, f"fceux_idx_{n}.bin"), "rb").read()
            b = open(os.path.join(our_dir, f"idx_{n}.bin"), "rb").read()
        except OSError as e:
            print(f"frame {n}: missing ({e.filename})")
            worst = max(worst, 1)
            continue
        diff = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
        if not diff:
            print(f"frame {n}: identical")
            continue
        worst = 1
        rows = sorted({i // 256 for i in diff})
        i = diff[0]
        print(f"frame {n}: {len(diff)} pixels differ, rows {rows[0]}-{rows[-1]} ({len(rows)} rows); "
              f"first at x={i % 256} y={i // 256}: fceux {a[i]:02X} ours {b[i]:02X}")
    return worst


if __name__ == "__main__":
    sys.exit(main())
