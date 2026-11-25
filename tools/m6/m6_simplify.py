"""m6_simplify.py: readability pass over passing M6 C (private decompile; nothing here is committed or shipped).

  python tools/m6/m6_simplify.py [--routines N] [--only name,name] [--tries 2]

Worst lint score first (m6_lint.py). For each routine: the prompt has the routine facts, the 6502 listing, the
outputs callers use and its current passing C, and asks for a rewrite for a human reader. The rewrite must pass the
same differential test (corpus + random states); it replaces the file only if it passes AND its lint score is lower.
The previous version is kept next to it in the work folder. Every try is logged to runs.jsonl with tag "simplify".
"""
import argparse
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import m6_lint  # noqa: E402
import m6_prompt  # noqa: E402
import m6_run  # noqa: E402
import m6_test  # noqa: E402

INSTRUCTIONS = """Rewrite the function below for a human reader. It already passes the differential test; the rewrite
must still do exactly the same thing (same RAM writes and values, same hardware accesses in the same order, same
outputs: %s).
Make it read like hand-written C:
- use the RAM names (they are #defined addresses) instead of raw RAM(0x..) addresses wherever a name exists;
- do not track flags (m->c, m->z, m->n, m->v) except the outputs listed above: compute what is needed with plain
  C expressions (carry of an addition is (sum > 0xFF), compares are plain comparisons);
- loops instead of gotos where the structure allows; meaningful local variable names;
- short comments that say what the code does (not which 6502 instruction it was).
Answer with exactly one ```c block that starts with `void routine(M6 *m)`.

Current passing C:
```c
%s
```"""


def simplify(name, bank, addr, insns, mem, tries):
    messages, outs, r = m6_prompt.build(bank, addr)
    short = m6_run.short_prompt(messages)
    current = os.path.join(m6_test.PRIVATE, name + '.c')
    old_text = open(current, encoding='utf-8').read()
    old_code = '\n'.join(old_text.splitlines()[1:])
    old_score = m6_lint.total(m6_lint.score(current, outs), insns)
    user = short[1]['content'].replace('Write `void routine(M6 *m)` now.', '') + '\n\n' + INSTRUCTIONS % (
        ', '.join('m->' + o for o in outs) or 'none', old_code)
    base = [short[0], {'role': 'user', 'content': user}]
    work = os.path.join(m6_test.M6, 'work', 'b%d_%04x' % (bank, addr))
    os.makedirs(work, exist_ok=True)
    msgs = base
    for t in range(tries):
        content, reasoning, usage, timings, secs = m6_run.ask(msgs, m6_run.TEMPS[min(t, 2)], True, t > 0)
        code = m6_run.extract_c(content) or m6_run.extract_c(reasoning)
        entry = {'time': time.strftime('%Y-%m-%d %H:%M:%S'), 'routine': name, 'bank': bank, 'addr': '%04X' % addr,
                 'try': t + 1, 'tag': 'simplify', 'prompt_tokens': usage.get('prompt_tokens'),
                 'completion_tokens': usage.get('completion_tokens'), 'seconds': round(secs, 1), 'outputs': outs,
                 'finish_reason': usage.get('finish_reason')}
        if code is None:
            entry.update({'result': 'noanswer', 'message': 'no c block'})
            m6_run.log(entry)
            print('  try %d: no answer (%s tokens)' % (t + 1, usage.get('completion_tokens')))
            continue
        cand = os.path.join(work, 'simplify%d.c' % (t + 1))
        with open(cand, 'w', newline='\n') as f:
            f.write(code)
        rc, msg = m6_test.run_test(bank, addr, cand, outs or '-', work=work, show=m6_prompt.ram_reads(bank, addr),
                                   callees=r.get('callees'))
        new_score = m6_lint.total(m6_lint.score(cand, outs), insns) if rc == 0 else None
        result = 'pass' if rc == 0 else {1: 'mismatch', 3: 'compile'}.get(rc, 'setup')
        entry.update({'result': result, 'message': msg[:400], 'lint_before': round(old_score, 1),
                      'lint_after': new_score if new_score is None else round(new_score, 1)})
        m6_run.log(entry)
        print('  try %d: %s, lint %.1f -> %s (%s tokens, %.0f s) %s' % (
            t + 1, result, old_score, '-' if new_score is None else '%.1f' % new_score,
            usage.get('completion_tokens'), secs, msg[:110]))
        if rc == 0 and new_score < old_score:
            shutil.copy(current, os.path.join(work, 'before_simplify.c'))
            header = old_text.splitlines()[0].rstrip()
            if header.endswith('*/'):
                header = header[:-2].rstrip()
            with open(current, 'w', newline='\n') as f:
                f.write('%s Simplified, lint %.1f -> %.1f. */\n' % (header, old_score, new_score))
                f.write(code)
            return True, old_score, new_score
        if rc == 0:
            return False, old_score, new_score  # correct but not more readable: keep the old one
        msgs = [base[0], {'role': 'user', 'content': base[1]['content'] + (
            '\n\nYour previous rewrite:\n```c\n%s```\nfailed the differential test:\n%s\nFix it.' % (code, msg))}]
    return False, old_score, None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--routines', type=int, default=5)
    ap.add_argument('--only')
    ap.add_argument('--tries', type=int, default=2)
    ap.add_argument('--shard', default='0/1', help='i/n: only routines with (bank*0x10000+addr) %% n == i')
    a = ap.parse_args()
    info = m6_lint.routine_info()
    done_before = set()  # routines that already had a readability try: kept or replaced, not asked again
    if os.path.isfile(m6_run.LOG):
        import json
        for line in open(m6_run.LOG, encoding='utf-8'):
            if '"simplify"' in line:
                done_before.add(json.loads(line).get('routine'))
    rows = []
    for f in os.listdir(m6_test.PRIVATE):
        name = os.path.splitext(f)[0]
        if not f.endswith('.c') or name not in info:
            continue
        if a.only and name not in a.only.split(','):
            continue
        text = open(os.path.join(m6_test.PRIVATE, f), encoding='utf-8').readline()
        if 'Simplified' in text or (name in done_before and not a.only):
            continue
        bank, addr, insns, mem = info[name]
        si, sn = (int(x) for x in a.shard.split('/'))
        if (bank * 0x10000 + addr) % sn != si:
            continue
        outs, _ = m6_prompt.caller_outputs(bank, addr, mem)
        rows.append((m6_lint.total(m6_lint.score(os.path.join(m6_test.PRIVATE, f), outs), insns), name))
    rows.sort(reverse=True)
    done = []
    for sc, name in rows[:a.routines]:
        bank, addr, insns, mem = info[name]
        print('%s (bank %d $%04X), lint %.1f' % (name, bank, addr, sc))
        try:
            done.append((name,) + simplify(name, bank, addr, insns, mem, a.tries))
        except (OSError, ValueError, KeyError, RuntimeError) as e:
            print('  error: %r' % e)
    print('\nreplaced %d/%d' % (sum(1 for d in done if d[1]), len(done)))
    for name, ok, before, after in done:
        print('  %-20s %-8s lint %.1f -> %s' % (name, 'replaced' if ok else 'kept', before,
                                               '-' if after is None else '%.1f' % after))


if __name__ == '__main__':
    main()
