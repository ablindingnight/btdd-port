#!/usr/bin/env python3
"""check_all: every gate of the port in one command, one PASS/FAIL table at the end.

  python tools/check_all.py [--out DIR] [--jobs N] [--no-build] [--only NAME,NAME] [--skip NAME,NAME] [--list]

Builds with every compiler found (MSVC via Visual Studio's CMake, clang, MSYS2 gcc), then runs the gates in
parallel (--jobs, default 4, to leave cores free): RAM vs FCEUX on both TAS movies (2P old PPU with every
compiler, 1P new PPU), APU vs FCEUX's log, picture vs FCEUX's index dumps, save state and rewind round trips,
the M13 netplay test (two rollback peers over a lossy simulated link, whole 2P movie), the M7 mod tests (every other gate runs with mods compiled in but off; the native gate also checks a logic mod
gives the same run natively and interpreted),
CPU/APU unit tests, M5 native lockstep (btdd_gen + zig cc), the M6 harness self-test (needs a passing private
candidate, see below), and release_audit. Exit code 0 only if nothing FAILed (SKIP and WARN do not fail).

Reference data is read from ..\\Gamefiles (never copied); outputs (ROM-derived dumps, generated C, the native
exe) go to --out, default ..\\Gamefiles\\Movies\\check_all. Tool paths can be overridden with the environment
variables VS_CMAKE, CMAKE, CLANG, GCC, ZIG, SH. The M6 self-test uses private/m6/sub_8624.c from this tree or
the file named by M6_CANDIDATE (private decompiles are never in git).
"""
import argparse
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BATTLE = os.path.dirname(REPO)
GAME = os.path.join(BATTLE, 'Gamefiles')
MOV = os.path.join(GAME, 'Movies')
ROM = os.path.join(GAME, 'Battletoads-Double Dragon (USA).nes')
M2P = os.path.join(MOV, 'tasvideos-4323M-2p.fm2')
M1P = os.path.join(MOV, 'tasvideos-2312S-1p.fm2')
REF2P = os.path.join(MOV, 'm1', 'fceux_2p.bin')
REF1P = os.path.join(MOV, 'm1', 'fceux_1p.bin')
APU_REF = os.path.join(MOV, 'musiclog', 'full', 'apu.csv')
SHOTS = {'2p': os.path.join(MOV, 'm2', '2p'), '1p': os.path.join(MOV, 'm2', '1p')}
TESTROMS = os.path.join(BATTLE, 'tools', 'testroms')
EXE = '.exe' if os.name == 'nt' else ''


def first_existing(*paths):
    for p in paths:
        if p and (os.path.isfile(p) or shutil.which(p)):
            return p
    return None


VS_CMAKE = first_existing(os.environ.get('VS_CMAKE'),
                          r'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions'
                          r'\Microsoft\CMake\CMake\bin\cmake.exe')
CMAKE = first_existing(os.environ.get('CMAKE'), 'cmake', r'C:\Program Files\CMake\bin\cmake.exe', VS_CMAKE)
CLANG = first_existing(os.environ.get('CLANG'), r'C:\Program Files\LLVM\bin\clang.exe', 'clang')
GCC = first_existing(os.environ.get('GCC'), r'C:\msys64\ucrt64\bin\gcc.exe', 'gcc')
ZIG = first_existing(os.environ.get('ZIG'), 'zig')
SH = first_existing(os.environ.get('SH'), 'sh', r'C:\Program Files\Git\bin\sh.exe')

# build folder -> (label, configure args)
BUILDS = [
    ('msvc', 'build', 'Release'),
    ('clang', 'build-clang', ''),
    ('gcc', 'build-gcc', ''),
]


def run(cmd, timeout=3600, env=None, cwd=REPO):
    """-> (returncode, combined output)"""
    try:
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env,
                           errors='replace')
        return r.returncode, r.stdout + r.stderr
    except subprocess.TimeoutExpired:
        return -1, 'timeout after %d s' % timeout
    except OSError as e:
        return -1, str(e)


def exe(build, name):
    label, folder, sub = build
    return os.path.join(REPO, folder, sub, name + EXE)


def ramdiff(ref, ours):
    code, out = run([sys.executable, os.path.join(REPO, 'tools', 'ramdiff.py'), ref, ours])
    line = out.strip().splitlines()[0] if out.strip() else 'no output'
    return ('PASS' if line.startswith('identical') else 'FAIL'), line


# ---------------- builds ----------------

def build_one(build):
    label, folder, sub = build
    env = dict(os.environ)
    if label == 'msvc':
        if os.name != 'nt' or not VS_CMAKE:
            return 'SKIP', 'Visual Studio CMake not found'
        conf = [VS_CMAKE, '-S', REPO, '-B', os.path.join(REPO, folder)]
        bld = [VS_CMAKE, '--build', os.path.join(REPO, folder), '--config', 'Release', '--', '-m:4']
    else:
        cc = CLANG if label == 'clang' else GCC
        if not cc or not CMAKE:
            return 'SKIP', '%s or cmake not found' % label
        env['PATH'] = os.path.dirname(cc) + os.pathsep + env.get('PATH', '')
        conf = [CMAKE, '-S', REPO, '-B', os.path.join(REPO, folder), '-G', 'Ninja',
                '-DCMAKE_C_COMPILER=' + cc.replace('\\', '/'), '-DCMAKE_BUILD_TYPE=Release']
        bld = [CMAKE, '--build', os.path.join(REPO, folder), '-j4']
    code, out = run(conf, env=env)
    if code != 0:
        return 'FAIL', 'configure failed: ' + out.strip().splitlines()[-1][:150]
    code, out = run(bld, env=env)
    if code != 0:
        errs = [l for l in out.splitlines() if 'error' in l.lower()]
        return 'FAIL', (errs[0] if errs else out.strip().splitlines()[-1])[:200]
    warn = sorted({l.strip() for l in out.splitlines()
                   if re.search(r'warning', l, re.I) and '_deps' not in l and 'SDL' not in l
                   and re.search(r'\.(c|h)[(:]', l)})
    if warn:
        return 'WARN', '%d warning(s) in our code, first: %s' % (len(warn), warn[0][-160:])
    return 'PASS', 'all targets, no warnings in our code'


# ---------------- gates ----------------

def gate_ram(build, movie, ref, ppu, out):
    def g():
        dump = os.path.join(out, 'ram_%s_%s.bin' % (build[0], ppu))
        code, o = run([exe(build, 'btdd_headless'), ROM, '--movie', movie, '--ppu', ppu, '--dump-ram', dump,
                       '--dump-every', '1', '--quiet'])
        if code != 0:
            return 'FAIL', 'btdd_headless exit %d: %s' % (code, o.strip()[-150:])
        return ramdiff(ref, dump)
    return g


def gate_apu(build, out):
    def g():
        csv = os.path.join(out, 'apu_2p.csv')
        ram = os.path.join(out, 'ram_2p_apu.bin')
        code, o = run([exe(build, 'btdd_apu_check'), ROM, '--movie', M2P, '--csv', csv, '--dump-ram', ram])
        if code != 0:
            return 'FAIL', 'btdd_apu_check exit %d' % code
        code, o = run([sys.executable, os.path.join(REPO, 'tools', 'apudiff.py'), APU_REF, csv])
        line = o.strip().splitlines()[0] if o.strip() else 'no output'
        st, rline = ramdiff(REF2P, ram)
        ok = line.startswith('identical') and st == 'PASS'
        return ('PASS' if ok else 'FAIL'), 'apudiff ' + line + '; RAM with APU: ' + rline.split(':')[0]
    return g


def gate_shots(build, which, out):
    def g():
        ref = SHOTS[which]
        frames = sorted(int(m.group(1)) for f in os.listdir(ref) for m in [re.match(r'fceux_idx_(\d+)\.bin$', f)] if m)
        d = os.path.join(out, 'shots_' + which)
        os.makedirs(d, exist_ok=True)
        cmd = [exe(build, 'btdd_shot'), ROM, '--movie', M2P if which == '2p' else M1P, '--frames',
               ','.join(map(str, frames)), '--out', d]
        if which == '1p':
            cmd[4:4] = ['--ppu', 'new']
        code, o = run(cmd)
        if code != 0:
            return 'FAIL', 'btdd_shot exit %d' % code
        code, o = run([sys.executable, os.path.join(REPO, 'tools', 'shotdiff.py'), ref, d])
        same = o.count(': identical')
        return ('PASS' if code == 0 and same == len(frames) else 'FAIL'), '%d/%d frames identical' % (same, len(frames))
    return g


def gate_state(build, out):
    def g():
        st = os.path.join(out, 'state_2p_30000.state')
        dump = os.path.join(out, 'ram_2p_state.bin')
        h = exe(build, 'btdd_headless')
        code, o = run([h, ROM, '--movie', M2P, '--frames', '30000', '--save-state', '30000', st, '--quiet'])
        if code != 0:
            return 'FAIL', 'save run exit %d' % code
        code, o = run([h, ROM, '--movie', M2P, '--load-state', st, '--dump-ram', dump, '--dump-every', '1', '--quiet'])
        if code != 0:
            return 'FAIL', 'load run exit %d: %s' % (code, o.strip()[-120:])
        s, line = ramdiff(REF2P, dump)
        return s, 'saved at 30000, loaded, continued: ' + line
    return g


def gate_rewind(build, out):
    def g():
        dump = os.path.join(out, 'ram_2p_rewind.bin')
        code, o = run([exe(build, 'rewind_test'), ROM, M2P, '--at', '30000', '--back', '600', '--dump-ram', dump])
        if code != 0:
            return 'FAIL', (o.strip().splitlines() or ['exit %d' % code])[-1]
        s, line = ramdiff(REF2P, dump)
        return s, '600 frames back exact, audio identical; continued: ' + line
    return g


def gate_unit(build):
    def g():
        res = []
        nt = os.path.join(TESTROMS, 'nestest.nes')
        if os.path.isfile(nt):
            code, o = run([exe(build, 'nestest_check'), nt, os.path.join(TESTROMS, 'nestest.log')])
            ok = code == 0 and '00 00 = official tests passed' in o
            res.append(('nestest', ok))
        for t in ('cpu_call_test', 'apu_synth_test'):
            code, o = run([exe(build, t)])
            res.append((t, code == 0 and 'FAIL' not in o))
        bad = [n for n, ok in res if not ok]
        return ('FAIL' if bad else 'PASS'), ('failed: ' + ', '.join(bad)) if bad else ', '.join(n for n, _ in res)
    return g


def gate_native(build, out):
    def g():
        if not ZIG or not SH:
            return 'SKIP', 'zig or sh not found'
        gen = os.path.join(out, 'generated')
        os.makedirs(gen, exist_ok=True)
        code, o = run([exe(build, 'btdd_gen'), ROM, os.path.join(REPO, 'meta', 'btdd.meta'), gen])
        if code != 0:
            return 'FAIL', 'btdd_gen exit %d' % code
        env = dict(os.environ, GEN=gen, OUT=out)
        code, o = run([SH, 'recomp/build_native.sh'], env=env)
        if code != 0:
            return 'FAIL', 'build_native failed: ' + o.strip()[-150:]
        code, o = run([SH, 'recomp/lockstep.sh', 'both', out], env=dict(os.environ, EXE='btdd_native'))
        n = len(re.findall(r'^identical:', o, re.M))
        if n != 2:
            return 'FAIL', o.strip()[-200:]
        # M7: a logic mod (lives=7, wrap hook) must give the same run under native dispatch as in the interpreter
        a, b = os.path.join(out, 'mods_interp.bin'), os.path.join(out, 'mods_native.bin')
        for x, path in ((exe(build, 'btdd_headless'), a), (os.path.join(out, 'btdd_native' + EXE), b)):
            code, o = run([x, ROM, '--movie', M2P, '--mod', 'lives=7', '--dump-ram', path, '--dump-every', '1', '--quiet'])
            if code != 0:
                return 'FAIL', 'mod run exit %d: %s' % (code, o.strip()[-120:])
        s, line = ramdiff(a, b)
        return s, '2P and 1P RAM identical; with mod lives=7 native == interpreter: ' + line.split(':')[0]
    return g


def gate_netplay(build):
    """M13: two rollback peers over a simulated 100 ms / 20 ms jitter / 5 % loss link play the whole 2P movie; every
    confirmed frame on both peers must equal the solo run."""
    def g():
        code, o = run([exe(build, 'nettest'), ROM, M2P, '--ping', '100', '--jitter', '20', '--loss', '5'])
        lines = o.strip().splitlines()
        if code != 0 or not lines or lines[-1] != 'PASS':
            return 'FAIL', (lines[-1] if lines else 'no output')[:200]
        rb = re.findall(r'rollbacks (\d+)', o)
        return 'PASS', 'whole 2P movie, 100 ms + 20 ms jitter + 5 %% loss: both peers == solo run (rollbacks %s)' % \
            '/'.join(rb)
    return g


def gate_mods(build):
    def g():
        code, o = run([exe(build, 'mods_test'), ROM, M2P, M1P, '--frames', '20000'])
        fails = [l for l in o.splitlines() if l.startswith('FAIL:')]
        if code != 0:
            return 'FAIL', (fails[0] if fails else o.strip()[-150:])[:200]
        return 'PASS', 'lives (after/before/replace hooks), nolimit (RAM unchanged, sprites added) on both movies'
    return g


def gate_m6(out):
    def g():
        cand = os.environ.get('M6_CANDIDATE') or os.path.join(REPO, 'private', 'm6', 'sub_8624.c')
        if not os.path.isfile(cand):
            return 'SKIP', 'no private candidate (set M6_CANDIDATE to a passing sub_8624.c)'
        if not ZIG:
            return 'SKIP', 'zig not found'
        t = os.path.join(REPO, 'tools', 'm6', 'm6_test.py')
        code, o = run([sys.executable, t, '0:8624', cand, '--work', os.path.join(out, 'm6_good')])
        good = code == 0 and 'PASS' in o
        broken = os.path.join(out, 'm6_broken.c')
        src = open(cand, encoding='utf-8').read()
        mutated = src.replace('a ^= v2f;', 'a ^= v2f ^ 1;', 1)
        if mutated == src:  # generic mutation: flip the first hex constant
            mutated = re.sub(r'0x([0-9A-Fa-f]{2})\b', lambda m: '0x%02X' % (int(m.group(1), 16) ^ 1), src, 1)
        open(broken, 'w', encoding='utf-8').write(mutated)
        code2, o2 = run([sys.executable, t, '0:8624', broken, '--work', os.path.join(out, 'm6_broken')])
        caught = code2 == 1 and 'FAIL' in o2
        ok = good and caught
        return ('PASS' if ok else 'FAIL'), ('good candidate passes, mutated one fails' if ok else
                                           'good: %s / mutated: %s' % (o.strip()[-80:], o2.strip()[-80:]))
    return g


def gate_audit():
    code, o = run([sys.executable, os.path.join(REPO, 'tools', 'release_audit.py')])
    line = [l for l in o.splitlines() if l.startswith('release_audit')]
    line = line[-1] if line else o.strip()[-150:]
    return ('PASS' if code == 0 and 'CLEAN' in line else 'FAIL'), line


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=os.path.join(MOV, 'check_all'))
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--no-build', action='store_true')
    ap.add_argument('--only', default='')
    ap.add_argument('--skip', default='')
    ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    t0 = time.time()
    results = []

    def want(name):
        only = [x for x in a.only.split(',') if x]
        skip = [x for x in a.skip.split(',') if x]
        return (not only or any(name.startswith(o) for o in only)) and not any(name.startswith(s) for s in skip)

    built = {}
    for b in BUILDS:
        name = 'build-' + b[0]
        if a.list:
            print(name)
            continue
        if a.no_build or not want(name):
            built[b[0]] = os.path.isfile(exe(b, 'btdd_headless'))
            continue
        print('building with %s ...' % b[0], flush=True)
        s, d = build_one(b)
        results.append((name, s, d))
        built[b[0]] = s in ('PASS', 'WARN')
    if a.list:  # name every gate as if all builds existed
        built = {b[0]: True for b in BUILDS}
    main_build = next((b for b in BUILDS if built.get(b[0])), None)

    gates = []
    if main_build:
        m = main_build
        gates += [('ram-2p-old-' + m[0], gate_ram(m, M2P, REF2P, 'old', out)),
                  ('ram-1p-new-' + m[0], gate_ram(m, M1P, REF1P, 'new', out)),
                  ('apu-2p', gate_apu(m, out)),
                  ('shots-2p', gate_shots(m, '2p', out)),
                  ('shots-1p', gate_shots(m, '1p', out)),
                  ('state-2p', gate_state(m, out)),
                  ('rewind-2p', gate_rewind(m, out)),
                  ('mods-test', gate_mods(m)),
                  ('netplay', gate_netplay(m)),
                  ('native-lockstep', gate_native(m, out)),
                  ('m6-selftest', gate_m6(out))]
        for b in BUILDS:
            if b is not m and built.get(b[0]):
                gates.append(('ram-2p-old-' + b[0], gate_ram(b, M2P, REF2P, 'old', out)))
        for b in BUILDS:
            if built.get(b[0]):
                gates.append(('unit-' + b[0], gate_unit(b)))
    gates.append(('release-audit', gate_audit))
    if a.list:
        for n, _ in gates:
            print(n)
        return 0
    gates = [(n, f) for n, f in gates if want(n)]
    if not main_build:
        results.append(('builds', 'FAIL', 'no build has btdd_headless; nothing to run'))

    print('running %d gates, %d at a time (outputs in %s) ...' % (len(gates), a.jobs, out), flush=True)
    with cf.ThreadPoolExecutor(max_workers=max(1, a.jobs)) as pool:
        futs = {pool.submit(f): n for n, f in gates}
        done = {}
        for fu in cf.as_completed(futs):
            n = futs[fu]
            try:
                done[n] = fu.result()
            except Exception as e:  # a gate crashing is a FAIL, not a crash of the whole run
                done[n] = ('FAIL', 'exception: %s' % e)
            print('  %-20s %s' % (n, done[n][0]), flush=True)
    results += [(n,) + done[n] for n, _ in gates]

    w = max(len(r[0]) for r in results)
    print('\n%-*s  %-6s %s' % (w, 'gate', 'result', 'detail'))
    print('-' * (w + 60))
    for n, s, d in results:
        print('%-*s  %-6s %s' % (w, n, s, d))
    fails = sum(1 for r in results if r[1] == 'FAIL')
    print('-' * (w + 60))
    print('%s: %d gates, %d FAIL, %d WARN, %d SKIP, %.0f s' % ('PASS' if not fails else 'FAIL', len(results), fails,
          sum(1 for r in results if r[1] == 'WARN'), sum(1 for r in results if r[1] == 'SKIP'), time.time() - t0))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
