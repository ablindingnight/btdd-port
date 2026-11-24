"""m6_recheck.py: re-run the differential test on every passing private/m6 file with the current harness.

  python tools/m6/m6_recheck.py [--fuzz 0] [--only name,name]

Use after a harness change (names header, callee handling, far calls) to confirm nothing that passed before
breaks. Prints one line per routine and the list of failures; exit code 1 if any fail. No model involved.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import m6_prompt  # noqa: E402
import m6_test  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--fuzz', type=int, default=0)
    ap.add_argument('--only')
    a = ap.parse_args()
    by_name = {r['name']: r for r in m6_prompt.m6_pick.routines()}
    bad = []
    files = sorted(f for f in os.listdir(m6_test.PRIVATE) if f.endswith('.c'))
    for f in files:
        name = f[:-2]
        if a.only and name not in a.only.split(','):
            continue
        r = by_name.get(name)
        head = re.match(r'/\* \S+: bank (\d) \$([0-9A-F]{4})', open(os.path.join(m6_test.PRIVATE, f), encoding='utf-8').readline())
        if head:  # names repeat across banks: the header says which one
            r = next((x for x in m6_prompt.m6_pick.routines()
                      if x['bank'] == int(head.group(1)) and x['start'] == int(head.group(2), 16)), r)
        if r is None:
            print('%-24s no such routine in btdd.meta' % name)
            bad.append(name)
            continue
        msgs, outs, info = m6_prompt.build(r['bank'], r['start'])
        work = os.path.join(m6_test.M6, 'work', 'recheck', name)
        rc, msg = m6_test.run_test(r['bank'], r['start'], os.path.join(m6_test.PRIVATE, f), outs or '-', work=work,
                                   fuzz=a.fuzz, callees=info.get('callees'))
        print('%-24s %s' % (name, msg.splitlines()[0][:110] if msg else rc), flush=True)
        if rc != 0:
            bad.append(name)
    print('\n%d checked, %d failed%s' % (len(files) if not a.only else len(a.only.split(',')), len(bad),
                                         (': ' + ' '.join(bad)) if bad else ''))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
