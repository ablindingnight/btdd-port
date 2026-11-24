"""m6_test.py: differential test of one candidate C routine against the original 6502 code.

  python tools/m6/m6_test.py BANK:ADDR candidate.c [--outputs a,x,c] [--corpus DIR ...] [--work DIR]

Compiles tools/m6/m6_harness.c + the candidate (+ the reference interpreter) with zig cc (CC overrides), then runs
it over the routine's corpus files (default: both movies under ..\\Gamefiles\\Movies\\m6\\corpus). Prints the
harness's last line: "PASS n/n ..." or one compact "FAIL state ..." line. Exit code: 0 pass, 1 mismatch,
2 setup problem, 3 the candidate does not compile (the compiler's first errors are printed).
m6_names.h (RAM names from meta/btdd.meta, reviewed + proposed) is written into the work folder.
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
M6 = os.path.join(REPO, '..', 'Gamefiles', 'Movies', 'm6')
ROM = os.path.join(REPO, '..', 'Gamefiles', 'Battletoads-Double Dragon (USA).nes')
CC = os.environ.get('CC', 'zig cc').split()


def ram_names(meta=os.path.join(REPO, 'meta', 'btdd.meta')):
    """-> [(name, addr, size, kind, naming)] unique names (first wins), skipping aliases."""
    out, seen = [], set()
    for line in open(meta, encoding='utf-8'):
        f = line.split()
        if not f or f[0] != 'ram':
            continue
        name = f[3]
        kv = dict(x.split('=', 1) for x in f[4:] if '=' in x)
        if name in seen or not re.match(r'^[A-Za-z_]\w*$', name) or kv.get('kind') in ('alias', 'io'):
            continue
        seen.add(name)
        out.append((name, int(f[1][1:], 16), int(f[2]), kv.get('kind'), kv.get('naming')))
    return out


def write_names(path):
    lines = ['/* RAM names from meta/btdd.meta (addresses only). naming=proposed names are unreviewed hints. */',
             '#ifndef M6_NAMES_H', '#define M6_NAMES_H']
    for name, addr, size, kind, naming in ram_names():
        lines.append('#define %-24s 0x%04X /* %s, %d byte(s), %s */' % (name, addr, kind, size, naming))
    lines.append('#endif')
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')


def corpus_files(bank, addr, dirs):
    out = []
    for d in dirs:
        p = os.path.join(d, 'b%d_%04x.m6c' % (bank, addr))
        if os.path.isfile(p):
            out.append(p)
    return out


def build(candidate, work):
    os.makedirs(work, exist_ok=True)
    write_names(os.path.join(work, 'm6_names.h'))
    exe = os.path.join(work, 'm6_harness.exe')
    src = os.path.join(REPO, 'src')
    cmd = CC + ['-O1', '-std=c11', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-I' + src, '-I' + HERE, '-I' + work,
                '-include', 'm6_api.h', '-include', 'm6_names.h',
                os.path.join(HERE, 'm6_harness.c'), candidate,
                os.path.join(src, 'runtime', 'cpu.c'), os.path.join(src, 'rom', 'rom.c'),
                os.path.join(src, 'rom', 'sha1.c'), '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        errs = [l for l in (r.stderr + r.stdout).splitlines() if 'error' in l][:5]
        return None, '\n'.join(errs) or (r.stderr + r.stdout)[-800:]
    return exe, ''


def run_test(bank, addr, candidate, outputs='-', dirs=None, work=None, fuzz=2000, show=None):
    """-> (code, message). code 0 pass, 1 mismatch, 2 setup, 3 compile error."""
    dirs = dirs or [os.path.join(M6, 'corpus', '2p'), os.path.join(M6, 'corpus', '1p')]
    work = work or os.path.join(M6, 'work')
    files = corpus_files(bank, addr, dirs)
    if not files:
        return 2, 'no corpus for bank %d $%04X' % (bank, addr)
    exe, err = build(candidate, work)
    if not exe:
        return 3, 'COMPILE ERROR: ' + err
    env = dict(os.environ, M6_FUZZ=str(fuzz), M6_SHOW=','.join('%04X' % x for x in (show or [])))
    r = subprocess.run([exe, ROM, str(bank), '%04X' % addr, outputs or '-'] + files, capture_output=True,
                       text=True, timeout=600, env=env)
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    msg = lines[0] if lines and lines[0].startswith('FAIL') else (lines[-1] if lines else r.stderr.strip())
    return r.returncode, msg


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('routine', help='BANK:ADDR, e.g. 0:8624')
    ap.add_argument('candidate')
    ap.add_argument('--outputs', default='-')
    ap.add_argument('--corpus', action='append')
    ap.add_argument('--work')
    ap.add_argument('--fuzz', type=int, default=2000, help='random entry states after the corpus (0 = none)')
    a = ap.parse_args()
    b, ad = a.routine.split(':')
    code, msg = run_test(int(b), int(ad, 16), a.candidate, a.outputs, a.corpus, a.work, a.fuzz)
    print(msg)
    sys.exit(code)


if __name__ == '__main__':
    main()
