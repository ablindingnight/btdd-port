"""explore.py: run btdd_explore on every CPU core from a folder of snapshots and merge the results.

  python tools/explore.py --snaps DIR[,DIR2] --out OUTDIR --seconds 600 [--workers 8] [--seed 1]
                          [--known DEBUGGER_DIR] [--ram-pcs FILE] [--publish NAME] [--extra "--god-ratio 0.7"]

Writes OUTDIR/wN.cdl, wN-m.cdl, wN-finds.tsv per worker, OUTDIR/merged.cdl (FCEUX format) and
OUTDIR/merged-m.cdl (Mesen format, with jump/entry bits). --publish NAME copies them to
DEBUGGER_DIR/auto-NAME.cdl and auto-NAME-m.cdl so btdd-decomp's tools merge them. Everything here is
ROM-derived: OUTDIR must be outside the repo (e.g. ..\\Gamefiles\\Movies\\coverage\\...).
"""
import argparse, glob, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
GAME = os.path.normpath(os.path.join(ROOT, '..', 'Gamefiles'))
SIZE = 0x40000


def find_exe():
    for b in ('build-clang', 'build/Release', 'build-gcc', 'build'):
        p = os.path.join(ROOT, b, 'btdd_explore.exe')
        if os.path.exists(p):
            return p
    sys.exit('btdd_explore.exe not built')


def merge(paths, mesen):
    out = bytearray(SIZE)
    for p in paths:
        raw = open(p, 'rb').read()
        body = raw[-SIZE:]
        for i, b in enumerate(body):
            if b:
                out[i] |= b
    if mesen:
        crc = open(paths[0], 'rb').read()[5:9] if paths else b'\0\0\0\0'
        return b'CDLv2' + crc + bytes(out)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--snaps', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--seconds', type=float, default=300)
    ap.add_argument('--workers', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--known', default=os.path.join(GAME, 'Debugger'))
    ap.add_argument('--ram-pcs', default=os.path.join(GAME, 'Movies', 'coverage', 'tas2p-ram-pcs.txt'))
    ap.add_argument('--publish', default=None)
    ap.add_argument('--extra', default='')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    snaps = []
    for d in a.snaps.split(','):
        snaps += sorted(glob.glob(os.path.join(d, '*.snap')))
    if not snaps:
        sys.exit('no snapshots in ' + a.snaps)
    lst = os.path.join(a.out, 'snaps.txt')
    open(lst, 'w').write('\n'.join(snaps) + '\n')
    known = os.path.join(a.out, 'known.txt')
    open(known, 'w').write('\n'.join(sorted(glob.glob(os.path.join(a.known, '*.cdl')))) + '\n')
    rom = os.path.join(GAME, 'Battletoads-Double Dragon (USA).nes')
    exe = find_exe()
    procs = []
    for w in range(a.workers):
        pre = os.path.join(a.out, 'w%d' % w)
        os.makedirs(pre + '-finds', exist_ok=True)
        cmd = [exe, rom, '--snaps', lst, '--out', pre, '--worker', str(w), '--workers', str(a.workers),
               '--seconds', str(a.seconds), '--seed', str(a.seed), '--known', known, '--ram-pcs', a.ram_pcs]
        cmd += a.extra.split()
        procs.append(subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True))
    for p in procs:
        out, _ = p.communicate()
        print(out.strip())
    f = sorted(glob.glob(os.path.join(a.out, 'w*.cdl')))
    fc = [p for p in f if not p.endswith('-m.cdl')]
    fm = [p for p in f if p.endswith('-m.cdl')]
    open(os.path.join(a.out, 'merged.cdl'), 'wb').write(merge(fc, False))
    open(os.path.join(a.out, 'merged-m.cdl'), 'wb').write(merge(fm, True))
    print('merged %d worker logs into %s' % (len(fc), a.out))
    if a.publish:
        shutil.copy(os.path.join(a.out, 'merged.cdl'), os.path.join(a.known, 'auto-%s.cdl' % a.publish))
        shutil.copy(os.path.join(a.out, 'merged-m.cdl'), os.path.join(a.known, 'auto-%s-m.cdl' % a.publish))
        print('published auto-%s.cdl / auto-%s-m.cdl' % (a.publish, a.publish))


if __name__ == '__main__':
    main()
