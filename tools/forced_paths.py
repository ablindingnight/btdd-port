"""forced_paths.py: replays and forced situations for coverage (docs/COVERAGE_PLAN.md ways 1 and 5).

  python tools/forced_paths.py --out OUTDIR [--only NAME,NAME] [--publish] [--jobs 8]

Runs btdd_headless once per scenario (in parallel), each writing OUTDIR/NAME.cdl, NAME-m.cdl,
NAME-reads.tsv, NAME.txt (its console output) and snapshots in OUTDIR/snap-NAME/ (for btdd_explore).
--publish copies the logs of runs that did not jam to ..\\Gamefiles\\Debugger\\auto-NAME*.cdl and
auto-reads-NAME.tsv. OUTDIR must be outside the repo (everything here is ROM-derived).
"""
import argparse, glob, os, shutil, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
GAME = os.path.normpath(os.path.join(ROOT, '..', 'Gamefiles'))
ROM = os.path.join(GAME, 'Battletoads-Double Dragon (USA).nes')
MOV = os.path.join(GAME, 'Movies')
M2 = os.path.join(MOV, 'tasvideos-4323M-2p.fm2')
M1 = os.path.join(MOV, 'tasvideos-2312S-1p.fm2')
START, SELECT = 0x08, 0x04


SCENARIOS = {
    # title screen and demo left alone
    'attract': ['--frames', '40000'],
    # Start every 5 s from power-on: the default menu choice (1P), then a player who never moves: deaths,
    # game over, continue screen, back to the title
    'start1p': ['--frames', '40000', '--press-every', '300', '%X' % START],
    # Select+Start pressed by both pads
    'selstart': ['--frames', '30000', '--press-every', '157', '%X,%X' % (SELECT | START, START)],
    # 2P TAS until stage 1 has started, then no lives and only Start presses: game over / continue in 2P
    'gameover2p': ['--movie', M2, '--movie-until', '1500', '--frames', '30000', '--poke', '11=00,12=00@1500',
                   '--press-every', '400', '%X,%X' % (START, START)],
    # 2P TAS with game mode B ($0016 = 2) forced right after the mode menu (the TAS picks mode A)
    'modeb': ['--movie', M2, '--poke', '16=02@700', '--frames', '59351'],
    # the 2P TAS and then 60,000 frames of no input: ending, credits, what comes after
    'ending': ['--movie', M2, '--frames', '120000'],
    # other movies (the 1P TAS needs the new-PPU timing model; its fm2 header does not say so)
    'tas1p': ['--movie', M1, '--ppu', 'new'],
    # FCEUX 2.6's new PPU (which we match exactly) desyncs this 2.1.0-era movie in stage 1; on the old-PPU
    # model it happens to get to stage 3 before desyncing, so both runs are kept
    'tas1p-oldppu': ['--movie', M1, '--ppu', 'old'],
    # extra TASVideos movies (Movies/tasvideos-extra/movies/SOURCES.md): feos 2P 2011 routes that use deaths
    'feos-2p': ['--movie', os.path.join(MOV, 'tasvideos-extra', 'movies', 'sub3154-feos-battletoadsdoubledragon-2p.fm2')],
    'feos-2p-trunc': ['--movie', os.path.join(MOV, 'tasvideos-extra', 'movies', 'uf-feos-2p-Truncated.fm2')],
    # evgs TAS Editor project, converted with tools/fm3tofm2.py (a glitch route: level 9 by frame 1,275)
    'lamp': ['--movie', os.path.join(MOV, 'tasvideos-extra', 'movies', 'uf-btdd-lamp-conv.fm2')],
    # 2005 1P FCEU movie converted with tools/fcm2fm2.py: stage 1 and part of stage 2 before it desyncs (the
    # other one, sub553, desyncs on the title screen on both PPU models and is not used)
    'sub635': ['--movie', os.path.join(MOV, 'tasvideos-extra', 'movies', 'sub635-conv.fm2'), '--ppu', 'new'],
    # user movies: both end in a glitch that jams the CPU (koh-btdd at frame 38,894, koh-wrongwarp at 32,399);
    # stop 300 frames before so no garbage execution is logged as code
    'koh-btdd': ['--movie', os.path.join(MOV, 'userfiles', 'koh-btdd.fm2'), '--frames', '38594'],
    'koh-wrongwarp': ['--movie', os.path.join(MOV, 'userfiles', 'koh-wrongwarp.fm2'), '--frames', '32099'],
}


def find_exe():
    for b in ('build-clang', 'build/Release', 'build-gcc', 'build'):
        p = os.path.join(ROOT, b, 'btdd_headless.exe')
        if os.path.exists(p):
            return p
    sys.exit('btdd_headless.exe not built')


def run(name, args, out, exe):
    sd = os.path.join(out, 'snap-' + name)
    os.makedirs(sd, exist_ok=True)
    cmd = [exe, ROM] + args + ['--cdl', os.path.join(out, name + '.cdl'),
                               '--cdl-mesen', os.path.join(out, name + '-m.cdl'),
                               '--reads', os.path.join(out, name + '-reads.tsv'),
                               '--indirect', os.path.join(out, name + '-indirect.tsv'),
                               '--snap-dir', sd, '--snap-every', '120']
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    open(os.path.join(out, name + '.txt'), 'w').write(' '.join(cmd) + '\n' + r.stdout)
    jam = 'jammed' in r.stdout
    return name, jam, r.stdout.strip().splitlines()[-3:] if r.stdout else []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', required=True)
    ap.add_argument('--only', default=None)
    ap.add_argument('--publish', action='store_true')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    exe = find_exe()
    names = a.only.split(',') if a.only else list(SCENARIOS)
    with ThreadPoolExecutor(a.jobs) as ex:
        res = list(ex.map(lambda n: run(n, SCENARIOS[n], a.out, exe), names))
    for name, jam, tail in res:
        print('%-14s %s' % (name, 'JAMMED (not published)' if jam else 'ok'))
        for l in tail:
            print('    ' + l)
        if a.publish and not jam:
            dbg = os.path.join(GAME, 'Debugger')
            shutil.copy(os.path.join(a.out, name + '.cdl'), os.path.join(dbg, 'auto-%s.cdl' % name))
            shutil.copy(os.path.join(a.out, name + '-m.cdl'), os.path.join(dbg, 'auto-%s-m.cdl' % name))
            shutil.copy(os.path.join(a.out, name + '-reads.tsv'), os.path.join(dbg, 'auto-reads-%s.tsv' % name))


if __name__ == '__main__':
    main()
