"""m6_stats.py: summary of M6 model runs (..\\Gamefiles\\Movies\\m6\\runs.jsonl).

  python tools/m6/m6_stats.py [--since "2026-11-24 01:39"] [--total 1143]

Per routine (its last series of tries counts): passed on try 1 / after retries / failed; failure kinds over all
failed tries; mean completion tokens and model seconds per routine; a projection for --total routines.
"""
import argparse
import collections
import json
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
LOG = os.path.join(os.path.dirname(os.path.dirname(HERE)), '..', 'Gamefiles', 'Movies', 'm6', 'runs.jsonl')


def kind(e):
    r, m = e['result'], e.get('message', '')
    if r == 'noanswer':
        return 'no answer (token cap / no code block)'
    if r == 'compile':
        return 'does not compile'
    if r == 'setup':
        return 'setup'
    if re.search(r'output [CZNV] ', m):
        return 'flag output (carry etc.)'
    if re.search(r'output [AXY] ', m):
        return 'register output'
    if 'RAM $' in m:
        return 'RAM value (arithmetic/indexing)'
    if 'read of' in m or 'write $' in m or 'hardware access' in m:
        return 'hardware access order/value'
    return 'other'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--since', default='')
    ap.add_argument('--tag', help='only tries logged with this M6_TAG')
    ap.add_argument('--total', type=int, default=1143)
    ap.add_argument('--log', default=LOG)
    a = ap.parse_args()
    rows = [json.loads(l) for l in open(a.log) if l.strip()]
    rows = [r for r in rows if r['time'] >= a.since and (a.tag is None or r.get('tag') == a.tag)]
    series = collections.OrderedDict()  # routine -> list of tries of its last series
    for r in rows:
        key = r['routine']
        if r['try'] == 1 or key not in series:
            series[key] = []
        series[key].append(r)
    first = sum(1 for s in series.values() if s[0]['result'] == 'pass')
    later = sum(1 for s in series.values() if s[-1]['result'] == 'pass' and s[0]['result'] != 'pass')
    failed = len(series) - first - later
    kinds = collections.Counter(kind(t) for s in series.values() for t in s if t['result'] != 'pass')
    tok = [sum(t['completion_tokens'] or 0 for t in s) for s in series.values()]
    sec = [sum(t['seconds'] for t in s) for s in series.values()]
    tries = [len(s) for s in series.values()]
    n = len(series)
    print('routines: %d   passed on try 1: %d (%.0f%%)   after retries: %d   failed: %d   pass rate %.0f%%' % (
        n, first, 100.0 * first / max(n, 1), later, failed, 100.0 * (first + later) / max(n, 1)))
    print('tries per routine: mean %.2f' % (sum(tries) / max(n, 1)))
    if sum(sec):
        print('passes per GPU-hour: %.1f' % ((first + later) / (sum(sec) / 3600.0)))
    print('per routine: mean %.0f completion tokens, %.0f s of model time (median %.0f s)' % (
        sum(tok) / max(n, 1), sum(sec) / max(n, 1), sorted(sec)[n // 2] if n else 0))
    print('failed tries by kind:')
    for k, c in kinds.most_common():
        print('  %3d  %s' % (c, k))
    if n:
        per = sum(sec) / n
        print('projection for %d routines at this rate: %.1f h of model time (one slot, serial); these were small '
              'leaf/near-leaf routines, so expect more for the rest' % (a.total, a.total * per / 3600))
    for name, s in series.items():
        print('  %-20s %-5s tries %d  %6d tok  %5.0f s  %s' % (
            name, s[-1]['result'], len(s), sum(t['completion_tokens'] or 0 for t in s), sum(t['seconds'] for t in s),
            '' if s[-1]['result'] == 'pass' else kind(s[-1])))


if __name__ == '__main__':
    main()
