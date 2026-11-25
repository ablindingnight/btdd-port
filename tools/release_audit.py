"""Release audit: fail if any file contains ROM content (docs/ROM_POLICY.md).

Checks every file for
  1. raw byte runs of >= WINDOW bytes that also occur in the ROM's PRG data (skipping low-variety runs such
     as $00/$FF fill), and
  2. text that spells ROM bytes as hex ("0x12, 0x34", "$12,$34", "12 34 56", ".byte $12") - a C array or a hex
     dump of ROM data is caught even though its raw bytes differ, and
  3. printable ASCII strings (>= 8 chars) that occur in the ROM.

Usage:
  python tools/release_audit.py [--rom PATH] [--staged] [PATH ...]
    no PATH      : every file git tracks (plus --staged: the staged versions of staged files)
    PATH ...     : these files / folders (e.g. a release folder before zipping it)
Exit 0 = clean, 1 = ROM content found, 2 = setup problem (ROM missing).
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
DEFAULT_ROM = os.path.join(REPO, '..', 'Gamefiles', 'Battletoads-Double Dragon (USA).nes')
WINDOW = 16          # raw byte run length that counts as a copy
MIN_DISTINCT = 5     # windows with fewer distinct byte values are fill/patterns, not evidence
HEX_WINDOW = 12      # hex-spelled runs are shorter in practice (tables in docs/code)
MIN_STRING = 8
FORBIDDEN_DIRS = {'private', 'generated', 'assets'}  # ROM-derived by definition (only their README.md is allowed)

HEX_TOKEN = re.compile(r'(?:0x|\$)?([0-9A-Fa-f]{2})(?![0-9A-Fa-f])')


def is_progression(w, slack=3):
    """True when all but `slack` bytes of the run form one constant-step sequence (0,1,2,3..., FF,FE,...,
    00,10,20..., '+,-./0123'). Such runs carry no ROM content: C runtimes and compilers emit identity/lookup
    tables like these in every exe, and they also occur in the ROM, so they are not evidence of copying."""
    best = run = 1
    for k in range(1, len(w) - 1):
        if ((w[k + 1] - w[k]) & 0xFF) == ((w[k] - w[k - 1]) & 0xFF):
            run += 1
        else:
            run = 1
        best = max(best, run)
    return best + 1 >= len(w) - slack


def _const_step(seg):
    return len(seg) < 3 or all(((seg[k + 1] - seg[k]) & 0xFF) == ((seg[1] - seg[0]) & 0xFF) for k in range(len(seg) - 1))


def is_pad_plus_counter(w):
    """True for a run of one byte value followed (or preceded) by a constant-step sequence, e.g.
    00 x9 then 01 02 03 04 05 06 07: compiler padding in front of a lookup table, no ROM content."""
    for k in range(1, len(w)):
        a, b = w[:k], w[k:]
        if (len(set(a)) == 1 and _const_step(b)) or (len(set(b)) == 1 and _const_step(a)):
            return True
    return False


def windows(data, n):
    out = set()
    for i in range(len(data) - n + 1):
        w = data[i:i + n]
        # skip low-information runs: few distinct values, one byte filling most of the run (padding followed by a
        # short counter, e.g. 00 x12 01 02 03 04), or a near constant-step sequence (C runtime lookup tables)
        if (len(set(w)) >= MIN_DISTINCT and max(w.count(b) for b in set(w)) < 10 and not is_progression(w)
                and not is_pad_plus_counter(w)):
            out.add(w)
    return out


def rom_strings(prg):
    return {m.group() for m in re.finditer(rb'[\x20-\x7e]{%d,}' % MIN_STRING, prg)
            if len(set(m.group())) >= 4 and not is_progression(m.group())}


def hex_runs(text):
    """Yield byte strings spelled as consecutive hex tokens on one line."""
    for line in text.splitlines():
        toks = [int(m.group(1), 16) for m in HEX_TOKEN.finditer(line)]
        if len(toks) >= HEX_WINDOW:
            yield bytes(toks)


def git_files(staged):
    names = subprocess.run(['git', 'ls-files', '-z'], cwd=REPO, capture_output=True, check=True).stdout
    files = [n.decode() for n in names.split(b'\0') if n]
    if staged:
        diff = subprocess.run(['git', 'diff', '--cached', '--name-only', '--diff-filter=ACMR', '-z'],
                              cwd=REPO, capture_output=True, check=True).stdout
        files = sorted(set(files) | {n.decode() for n in diff.split(b'\0') if n})
    return files


def read_git_or_disk(rel, staged):
    if staged:
        r = subprocess.run(['git', 'show', ':' + rel], cwd=REPO, capture_output=True)
        if r.returncode == 0:
            return r.stdout
    path = os.path.join(REPO, rel)
    return open(path, 'rb').read() if os.path.isfile(path) else None


def walk(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for n in names:
                    yield os.path.join(root, n)
        else:
            yield p


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rom', default=DEFAULT_ROM)
    ap.add_argument('--staged', action='store_true')
    ap.add_argument('paths', nargs='*')
    a = ap.parse_args()

    if not os.path.isfile(a.rom):
        print('release_audit: ROM not found at %s (needed to compare against)' % a.rom)
        return 2
    prg = open(a.rom, 'rb').read()[16:]
    raw_w = windows(prg, WINDOW)
    hex_w = windows(prg, HEX_WINDOW)
    strings = rom_strings(prg)

    if a.paths:
        items = [(f, (lambda f=f: open(f, 'rb').read())) for f in walk(a.paths)]
    else:
        items = [(f, (lambda f=f: read_git_or_disk(f, a.staged))) for f in git_files(a.staged)]

    hits = []
    if not a.paths:
        for name in git_files(a.staged):
            top = name.replace('\\', '/').split('/')[0]
            if top in FORBIDDEN_DIRS and not name.endswith('README.md'):
                hits.append('%s: files under %s/ must never be committed' % (name, top))
    for name, load in items:
        data = load()
        if not data:
            continue
        for i in range(len(data) - WINDOW + 1):
            if data[i:i + WINDOW] in raw_w:
                hits.append('%s: %d-byte ROM run at file offset %d' % (name, WINDOW, i))
                break
        text = data.decode('utf-8', 'ignore')
        for run in hex_runs(text):
            if any(run[i:i + HEX_WINDOW] in hex_w for i in range(len(run) - HEX_WINDOW + 1)):
                hits.append('%s: ROM bytes written out as hex text' % name)
                break
        for s in strings:
            if s in data:
                hits.append('%s: ROM string %r' % (name, s.decode()))
                break

    for h in hits:
        print('ROM CONTENT: ' + h)
    print('release_audit: %d file(s) checked, %s' % (len(items), 'CLEAN' if not hits else '%d problem(s)' % len(hits)))
    return 1 if hits else 0


if __name__ == '__main__':
    sys.exit(main())
