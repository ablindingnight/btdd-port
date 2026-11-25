"""m6_overnight.py: long unattended M6 run on both llama-server slots, with the readability pass in between.

  python tools/m6/m6_overnight.py --until 08:00 [--shards 2] [--chunk 10] [--simplify-every 50]

One thread per shard runs `m6_run.py --kinds <all> --new-only --shard i/n --routines CHUNK` again and again (each
process re-ranks best-first), so the two slots never work on the same routine and every slot has at most one
request in flight. Shard 0, between its chunks, runs `m6_simplify.py` on the worst-lint passing routines whenever
SIMPLIFY_EVERY new routines have passed since the last readability pass (its slot is free then, so the server still
sees at most one request per slot). A restarting server is waited for (M6_SERVER_WAIT, default here 2 h). Every
process is killed at the deadline. Stats by kind go to the log after every chunk. Log: ..\\Gamefiles\\Movies\\m6\\
overnight.log; tag of the model tries: M6_TAG (default "overnight").
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
import m6_test  # noqa: E402

KINDS = 'leaf,calls,tail,alt,far,dispatch'
LOCK = threading.Lock()


def say(log, msg):
    with LOCK:
        line = '%s %s' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg)
        print(line, flush=True)
        with open(log, 'a', encoding='utf-8') as f:
            f.write(line + '\n')


def passing():
    return sum(1 for f in os.listdir(m6_test.PRIVATE) if f.endswith('.c'))


def run(cmd, out, deadline, env):
    """Run until done or the deadline (then kill). -> (finished, output text)"""
    with open(out, 'a', encoding='utf-8') as f:
        p = subprocess.Popen(cmd, cwd=REPO, stdout=f, stderr=subprocess.STDOUT, env=env)
        try:
            p.wait(timeout=max(1, deadline - time.time()))
            return True
        except subprocess.TimeoutExpired:
            p.kill()
            p.wait()
            return False


def shard(i, a, deadline, log, state, env):
    out = os.path.join(m6_test.M6, 'overnight_s%d.log' % i)
    while time.time() < deadline - 120:
        before = os.path.getsize(out) if os.path.isfile(out) else 0
        n0 = passing()
        mode = ['--new-only'] if state['phase'][i] == 1 else ['--max-series', '1']
        cmd = [sys.executable, '-u', os.path.join(HERE, 'm6_run.py'), '--kinds', a.kinds] + mode + [
               '--max-size', str(a.max_size), '--routines', str(a.chunk), '--shard', '%d/%d' % (i, a.shards)]
        if not run(cmd, out, deadline, env):
            say(log, 'shard %d: stopped at the deadline' % i)
            return
        text = open(out, encoding='utf-8', errors='replace').read()[before:]
        tried = text.count(' kind ')
        say(log, 'shard %d: chunk done, %d routines tried, passing C now %d (+%d during the chunk)' % (
            i, tried, passing(), passing() - n0))
        if 'no more routines' in text and tried == 0:
            if state['phase'][i] == 1:  # every routine tried once: retry the ones that failed once
                state['phase'][i] = 2
                say(log, 'shard %d: no untried routines left; retrying routines that failed once' % i)
                continue
            say(log, 'shard %d: nothing left to try' % i)
            return
        if i == 0 and passing() - state['last_simplify'] >= a.simplify_every:
            state['last_simplify'] = passing()
            say(log, 'shard 0: readability pass (m6_simplify, %d routines)' % a.simplify_routines)
            run([sys.executable, '-u', os.path.join(HERE, 'm6_simplify.py'), '--routines', str(a.simplify_routines)],
                os.path.join(m6_test.M6, 'overnight_simplify.log'), deadline, env)
        stats = subprocess.run([sys.executable, os.path.join(HERE, 'm6_stats.py'), '--tag', env['M6_TAG'],
                                '--by-kind'], cwd=REPO, capture_output=True, text=True).stdout
        say(log, 'stats so far (tag %s):\n%s' % (env['M6_TAG'], stats.rstrip()))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--until', required=True, help='HH:MM, today or tomorrow, whichever comes first')
    ap.add_argument('--shards', type=int, default=2)
    ap.add_argument('--chunk', type=int, default=10)
    ap.add_argument('--kinds', default=KINDS)
    ap.add_argument('--max-size', type=int, default=512)
    ap.add_argument('--simplify-every', type=int, default=50)
    ap.add_argument('--simplify-routines', type=int, default=5)
    a = ap.parse_args()
    now = datetime.datetime.now()
    h, m = (int(x) for x in a.until.split(':'))
    end = now.replace(hour=h, minute=m, second=0, microsecond=0)
    if end <= now:
        end += datetime.timedelta(days=1)
    deadline = end.timestamp()
    log = os.path.join(m6_test.M6, 'overnight.log')
    env = dict(os.environ)
    env.setdefault('M6_TAG', 'overnight')
    env.setdefault('M6_SERVER_WAIT', '7200')
    state = {'last_simplify': passing(), 'phase': [1] * a.shards}
    say(log, 'start: until %s, %d shards, chunks of %d, kinds %s, max %d bytes, passing C %d' % (
        end.strftime('%Y-%m-%d %H:%M'), a.shards, a.chunk, a.kinds, a.max_size, passing()))
    threads = [threading.Thread(target=shard, args=(i, a, deadline, log, state, env)) for i in range(a.shards)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    stats = subprocess.run([sys.executable, os.path.join(HERE, 'm6_stats.py'), '--tag', env['M6_TAG']],
                           cwd=REPO, capture_output=True, text=True).stdout
    say(log, 'end: passing C %d\n%s' % (passing(), stats.rstrip()))


if __name__ == '__main__':
    main()
