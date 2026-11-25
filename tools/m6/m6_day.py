"""m6_day.py: unattended M6 day plan on both llama-server slots, phase by phase, until a deadline.

  python tools/m6/m6_day.py --until 14:30 [--shards 2] [--chunk 8]

Each shard (one process at a time per llama-server slot, --shard i/n so the shards never take the same routine)
walks the phases in order and moves on when a phase has nothing left for it:
  new       routines never tried that have a corpus (any movie or snapshot source), all supported kinds, best first
  random    routines with no corpus anywhere, tested on random entry states only (logged random_only)
  simplify  readability pass over every passing routine, worst lint score first (kept only if it still passes)
  retry     routines that failed once, first try with the 20k thinking budget (M6_THINK_BUDGET/M6_MAX_TOKENS)
The phase lists are re-ranked every chunk, so corpus captured while the run goes (new snapshot sources) is used
as soon as it lands. Every process is killed at the deadline. A restarting server is waited for (2 h).
Log: ..\\Gamefiles\\Movies\\m6\\day.log; tag of the model tries: M6_TAG (default "day").
"""
import argparse
import datetime
import os
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import m6_overnight  # noqa: E402  (say, run, passing)
import m6_test  # noqa: E402

PHASES = ('new', 'random', 'simplify', 'retry')


def chunk_cmd(phase, spec, a):
    run = [sys.executable, '-u', os.path.join(HERE, 'm6_run.py'), '--kinds', a.kinds, '--max-size', str(a.max_size),
           '--routines', str(a.chunk), '--shard', spec]
    if phase == 'new':
        return run + ['--new-only'], {}
    if phase == 'random':
        return run + ['--new-only', '--random-only'], {}
    if phase == 'retry':
        return run + ['--max-series', '1'], {'M6_THINK_BUDGET': '20000', 'M6_MAX_TOKENS': '24000'}
    return [sys.executable, '-u', os.path.join(HERE, 'm6_simplify.py'), '--routines', str(a.chunk),
            '--shard', spec], {}


def shard(i, a, deadline, log, env, plan):
    for phase, spec in plan:
        out = os.path.join(m6_test.M6, 'day_s%d_%s.log' % (i, phase))
        m6_overnight.say(log, 'shard %d: phase %s (routines %s)' % (i, phase, spec))
        while time.time() < deadline - 120:
            before = os.path.getsize(out) if os.path.isfile(out) else 0
            n0 = m6_overnight.passing()
            cmd, extra = chunk_cmd(phase, spec, a)
            if not m6_overnight.run(cmd, out, deadline, dict(env, **extra)):
                m6_overnight.say(log, 'shard %d: stopped at the deadline (phase %s)' % (i, phase))
                return
            text = open(out, encoding='utf-8', errors='replace').read()[before:]
            if phase == 'simplify':
                line = next((l.strip() for l in text.splitlines() if l.startswith('replaced ')), None)
                m6_overnight.say(log, 'shard %d: simplify chunk: %s' % (i, line or 'no summary (error?)'))
                if line is None or line == 'replaced 0/0':
                    break
                continue
            tried = text.count(' kind ')
            m6_overnight.say(log, 'shard %d: %s chunk, %d routines tried, passing C now %d (+%d)' % (
                i, phase, tried, m6_overnight.passing(), m6_overnight.passing() - n0))
            if tried == 0:
                break
        if time.time() >= deadline - 120:
            return
    m6_overnight.say(log, 'shard %d: all phases done' % i)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--until', required=True, help='HH:MM, today or tomorrow, whichever comes first')
    ap.add_argument('--shards', type=int, default=2)
    ap.add_argument('--chunk', type=int, default=8)
    ap.add_argument('--kinds', default=m6_overnight.KINDS)
    ap.add_argument('--max-size', type=int, default=512)
    ap.add_argument('--plan', help='per-shard phase lists, e.g. "new@0/1,random@0/1,retry@0/2;simplify@0/1,retry@1/2" '
                    '(phase@i/n: that phase on routines with (bank*0x10000+addr) %% n == i); default: every shard '
                    'runs all phases on its own i/n share')
    a = ap.parse_args()
    if a.plan:
        plans = [[(x.split('@')[0], x.split('@')[1]) for x in part.split(',')] for part in a.plan.split(';')]
        a.shards = len(plans)
    else:
        plans = [[(ph, '%d/%d' % (i, a.shards)) for ph in PHASES] for i in range(a.shards)]
    now = datetime.datetime.now()
    h, m = (int(x) for x in a.until.split(':'))
    end = now.replace(hour=h, minute=m, second=0, microsecond=0)
    if end <= now:
        end += datetime.timedelta(days=1)
    deadline = end.timestamp()
    log = os.path.join(m6_test.M6, 'day.log')
    env = dict(os.environ)
    env.setdefault('M6_TAG', 'day')
    env.setdefault('M6_SERVER_WAIT', '7200')
    m6_overnight.say(log, 'start: until %s, %d shards, chunks of %d, plan %s, passing C %d' % (
        end.strftime('%Y-%m-%d %H:%M'), a.shards, a.chunk,
        ' | '.join(' -> '.join('%s@%s' % x for x in p) for p in plans), m6_overnight.passing()))
    threads = [threading.Thread(target=shard, args=(i, a, deadline, log, env, plans[i])) for i in range(a.shards)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    stats = subprocess.run([sys.executable, os.path.join(HERE, 'm6_stats.py'), '--tag', env['M6_TAG'], '--by-kind'],
                           cwd=REPO, capture_output=True, text=True).stdout
    m6_overnight.say(log, 'end: passing C %d\n%s' % (m6_overnight.passing(), stats.rstrip()))


if __name__ == '__main__':
    main()
