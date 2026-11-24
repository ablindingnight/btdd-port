#!/usr/bin/env python3
"""Compare two per-frame APU channel logs (FCEUX musiclog.lua apu.csv vs btdd_apu_check --csv).

Rows are matched by the frame column. *_key columns are pitches printed with 2 decimals; they are compared
with a 0.011 tolerance (libm rounding can move the last digit). Every other column must match exactly.

  python tools/apudiff.py reference.csv candidate.csv [--max 20] [--from N] [--to N]
"""
import argparse
import csv
import sys


def load(path):
    with open(path, newline="") as f:
        r = csv.reader(f)
        header = next(r)
        rows = {}
        for row in r:
            if row:
                rows[int(row[0])] = row
    return header, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--max", type=int, default=20, help="differing rows to print")
    ap.add_argument("--from", dest="lo", type=int, default=0)
    ap.add_argument("--to", dest="hi", type=int, default=1 << 60)
    args = ap.parse_args()

    h1, a = load(args.reference)
    h2, b = load(args.candidate)
    if h1 != h2:
        print("headers differ:\n ", h1, "\n ", h2)
        return 2
    keys = [i for i, name in enumerate(h1) if name.endswith("_key")]
    per_col = [0] * len(h1)
    bad_rows = []
    compared = 0
    for fr in sorted(set(a) & set(b)):
        if fr < args.lo or fr > args.hi:
            continue
        compared += 1
        ra, rb = a[fr], b[fr]
        cols = []
        for i in range(1, len(h1)):
            x, y = ra[i], rb[i]
            if x == y:
                continue
            if i in keys:
                try:
                    if abs(float(x) - float(y)) <= 0.011:
                        continue
                except ValueError:
                    pass
            cols.append(i)
            per_col[i] += 1
        if cols:
            bad_rows.append((fr, cols))
    if not compared:
        print("no common frames")
        return 2
    if not bad_rows:
        print(f"identical: {compared} common rows compared (frames {min(set(a) & set(b))}-{max(set(a) & set(b))})")
        return 0
    print(f"{len(bad_rows)} of {compared} rows differ; first at frame {bad_rows[0][0]}")
    print("differing rows per column: " + ", ".join(f"{h1[i]}={n}" for i, n in enumerate(per_col) if n))
    for fr, cols in bad_rows[: args.max]:
        print(f" frame {fr}: " + "  ".join(f"{h1[i]} {a[fr][i]} vs {b[fr][i]}" for i in cols))
    return 1


if __name__ == "__main__":
    sys.exit(main())
