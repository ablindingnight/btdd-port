"""m6_test.py: differential test of one candidate C routine against the original 6502 code.

  python tools/m6/m6_test.py BANK:ADDR candidate.c [--outputs a,x,c] [--corpus DIR ...] [--work DIR]

Compiles tools/m6/m6_harness.c + the candidate (+ the reference interpreter) with zig cc (CC overrides), then runs
it over the routine's corpus files (default: both movies under ..\\Gamefiles\\Movies\\m6\\corpus). Prints the
harness's last line: "PASS n/n ..." or one compact "FAIL state ..." line. Exit code: 0 pass, 1 mismatch,
2 setup problem, 3 the candidate does not compile (the compiler's first errors are printed).
m6_names_N.h (RAM names from decomp HEAD, older btdd.meta names as aliases) is written per compile unit.
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
TIMEOUT = 120  # seconds for one harness run (normal runs take a few seconds)


_RAM_CACHE = {}


def ram_lines(meta=None):
    """btdd.meta-syntax ram records. Default: built from the decomp's current HEAD (src/ram.inc and
    docs/RAM_PROPOSALS.md, read through git show) with the meta exporter's own build_ram, so names graded after
    the meta export reach the prompts; falls back to meta/btdd.meta (M6_META_NAMES=1 forces it)."""
    if meta is None and not os.environ.get('M6_META_NAMES'):
        if 'head' not in _RAM_CACHE:
            try:
                sys.path.insert(0, os.path.join(REPO, 'tools'))
                import export_meta
                dec = os.path.join(REPO, '..', 'btdd-decomp')
                _RAM_CACHE['head'] = export_meta.build_ram(export_meta.git_show(dec, 'HEAD', 'src/ram.inc'),
                                                           export_meta.git_show(dec, 'HEAD', 'docs/RAM_PROPOSALS.md'))
            except (SystemExit, OSError, ImportError):
                _RAM_CACHE['head'] = None
        if _RAM_CACHE['head']:
            return _RAM_CACHE['head']
    meta = meta or os.path.join(REPO, 'meta', 'btdd.meta')
    return [l for l in open(meta, encoding='utf-8') if l.startswith('ram ')]


def ram_names(meta=None):
    """-> [(name, addr, size, kind, naming)] unique names (first wins), skipping aliases."""
    out, seen = [], set()
    for line in ram_lines(meta):
        f = line.split()
        if not f or f[0] != 'ram':
            continue
        name = f[3]
        kv = dict(x.split('=', 1) for x in f[4:] if '=' in x)
        if name in seen or not re.match(r'^[A-Za-z_]\w*$', name):  # aliases (ptr_xx_hi) and io names included
            continue
        seen.add(name)
        out.append((name, int(f[1][1:], 16), int(f[2]), kv.get('kind'), kv.get('naming')))
    return out


def write_names(path, exclude=()):
    lines = ['/* RAM names from meta/btdd.meta (addresses only). naming=proposed names are unreviewed hints. */',
             '#ifndef M6_NAMES_H', '#define M6_NAMES_H']
    names = ram_names()
    for name, addr, size, kind, naming in names:
        if name in exclude:  # a local of the same name in the code being built: the #define would break it
            continue
        lines.append('#define %-24s 0x%04X /* %s, %d byte(s), %s */' % (name, addr, kind, size, naming))
    have = {n for n, a, size, kind, naming in names}
    meta = os.path.join(REPO, 'meta', 'btdd.meta')
    if not os.environ.get('M6_META_NAMES') and os.path.isfile(meta):  # older names (passing C may use them)
        for name, addr, size, kind, naming in ram_names(meta):
            if name not in have and name not in exclude:
                have.add(name)
                lines.append('#define %-24s 0x%04X /* older name (btdd.meta) */' % (name, addr))
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


PRIVATE = os.path.join(REPO, 'private', 'm6')


def callee_sources(callees, work, bank=None, candidate=None):
    """One C file per callee: its passing C from private/m6 renamed to the callee's name, or a stub that runs the
    original code through m6_call. Plus m6_callees.h with the prototypes (included into every unit)."""
    files, protos = [], []
    callees = list(dict.fromkeys(callees))  # a callee reached by JSR and by a tail jump is one function
    if bank is not None:  # passing callee C calls its own callees: add them too (transitively)
        import m6_pick
        by_name = {r['name']: r['start'] for r in m6_pick.routines() if r['bank'] == bank}
        by_name.update({n: a for (b, a), n in m6_pick.entries().items() if b == bank})
        seen = {n for n, a in callees}
        i = -1 if candidate else 0  # -1: the candidate itself (older passing C may call e.g. sub_ffcb by name)
        while i < len(callees):
            mine = candidate if i < 0 else os.path.join(PRIVATE, callees[i][0] + '.c')
            if os.path.isfile(mine):
                for n in re.findall(r'\b([A-Za-z_]\w*)\s*\(\s*m\s*\)', open(mine, encoding='utf-8').read()):
                    if n in by_name and n not in seen:
                        seen.add(n)
                        callees.append((n, by_name[n]))
            i += 1
    for name, addr in callees:
        protos.append('void %s(M6 *m); /* $%04X */' % (name, addr))
        path = os.path.join(work, 'callee_%s.c' % name)
        mine = os.path.join(PRIVATE, name + '.c')
        with open(path, 'w', newline='\n') as f:
            if os.path.isfile(mine):
                f.write('#define routine %s\n#include "%s"\n#undef routine\n' % (name, mine.replace('\\', '/')))
            else:
                f.write('void %s(M6 *m) { m6_call(m, 0x%04X); }\n' % (name, addr))
        files.append(path)
    with open(os.path.join(work, 'm6_callees.h'), 'w', newline='\n') as f:
        f.write('\n'.join(protos) + '\n')
    return files


def build(candidate, work, callees=None, bank=None):
    os.makedirs(work, exist_ok=True)
    extra = callee_sources(callees or [], work, bank, candidate)
    units = []
    for i, unit in enumerate([candidate] + extra):  # each unit gets its own names header: a local in one
        text = open(unit, encoding='utf-8', errors='replace').read()  # must not drop a #define another needs
        for inc in re.findall(r'#include "([^"]+)"', text):
            if os.path.isfile(inc):
                text += open(inc, encoding='utf-8', errors='replace').read()
        decl = set(re.findall(r'\b(?:u?int(?:8|16|32)_t|int|unsigned|char|bool|long)\s+([A-Za-z_]\w*)', text))
        write_names(os.path.join(work, 'm6_names_%d.h' % i), decl)
        wrap = os.path.join(work, 'unit_%d.c' % i)
        with open(wrap, 'w', newline='\n') as f:
            f.write('#include "m6_names_%d.h"\n#include "%s"\n' % (i, os.path.abspath(unit).replace('\\', '/')))
        units.append(wrap)
    exe = os.path.join(work, 'm6_harness.exe')
    src = os.path.join(REPO, 'src')
    cmd = CC + ['-O1', '-std=c11', '-w', '-D_CRT_SECURE_NO_WARNINGS', '-I' + src, '-I' + HERE, '-I' + work,
                '-include', 'm6_api.h', '-include', 'm6_callees.h',
                os.path.join(HERE, 'm6_harness.c')] + units + [
                os.path.join(src, 'runtime', 'cpu.c'), os.path.join(src, 'rom', 'rom.c'),
                os.path.join(src, 'rom', 'sha1.c'), '-o', exe]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        errs = [l for l in (r.stderr + r.stdout).splitlines() if 'error' in l][:5]
        return None, '\n'.join(errs) or (r.stderr + r.stdout)[-800:]
    return exe, ''


def run_test(bank, addr, candidate, outputs='-', dirs=None, work=None, fuzz=2000, show=None, callees=None):
    """-> (code, message). code 0 pass, 1 mismatch, 2 setup, 3 compile error."""
    dirs = dirs or [os.path.join(M6, 'corpus', '2p'), os.path.join(M6, 'corpus', '1p')]
    work = work or os.path.join(M6, 'work')
    files = corpus_files(bank, addr, dirs)
    if not files:
        return 2, 'no corpus for bank %d $%04X' % (bank, addr)
    if 'm6_call' in open(candidate, encoding='utf-8', errors='replace').read():
        return 3, 'COMPILE ERROR: the routine must not use m6_call (that runs the original code); call callees by name'
    exe, err = build(candidate, work, callees, bank)
    if not exe:
        return 3, 'COMPILE ERROR: ' + err
    env = dict(os.environ, M6_FUZZ=str(fuzz), M6_SHOW=','.join('%04X' % x for x in (show or [])), M6_TRACE='1')
    out_path = os.path.join(work, 'harness_out.txt')
    with open(out_path, 'w') as out:
        p = subprocess.Popen([exe, ROM, str(bank), '%04X' % addr, outputs or '-'] + files, stdout=out,
                             stderr=subprocess.STDOUT, text=True, env=env)
        try:
            rc = p.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            rc = None
    lines = [l for l in open(out_path, errors='replace').read().splitlines() if l.strip()]
    if rc is None:
        last = next((l[2:] for l in reversed(lines) if l.startswith('@ ')), 'an unknown state')
        return 1, ('FAIL %s: the function did not return within %d s (endless loop? e.g. an 8-bit counter '
                   'compared with >= 0)' % (last, TIMEOUT))
    lines = [l for l in lines if not l.startswith('@ ')]
    msg = lines[0] if lines and lines[0].startswith('FAIL') else (lines[-1] if lines else '(no output)')
    return rc, msg


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
