"""m6_run.py: M6 driver. For each routine: prompt (m6_prompt.py) -> local model -> one C function -> differential
test (m6_test.py) -> retry with the failure line, up to --tries. Passing C goes to private/m6/ (gitignored:
the readable translations are the private decompile, never committed or shipped).

  python tools/m6/m6_run.py --routines 5 [--list FILE | --only 0:8624,6:81eb] [--tries 3]

The model is llama-server's OpenAI API (LLAMA_URL, default http://127.0.0.1:8080/v1/chat/completions), shared with
the decomp loop (one slot): requests simply queue. Thinking on, temperature 0.2 on the first try, 0.7 after.
Log: ..\\Gamefiles\\Movies\\m6\\runs.jsonl (one line per try: routine, try, tokens, seconds, result).
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import m6_prompt  # noqa: E402
import m6_test  # noqa: E402

API = os.environ.get('LLAMA_URL', 'http://127.0.0.1:8080/v1/chat/completions')
M6 = m6_test.M6
PRIVATE = os.path.join(REPO, 'private', 'm6')
LOG = os.path.join(M6, 'runs.jsonl')
TEMPS = [0.2, 0.7, 0.7]
MAX_TOKENS = 32000
SERVER_WAIT = 1800


def ask(messages, temperature):
    body = json.dumps({'messages': messages, 'temperature': temperature, 'max_tokens': MAX_TOKENS,
                       'cache_prompt': True, 'chat_template_kwargs': {'enable_thinking': True}}).encode()
    t0 = time.time()
    while True:
        try:
            req = urllib.request.Request(API, body, {'Content-Type': 'application/json'})
            with urllib.request.urlopen(req, timeout=3600) as r:
                resp = json.loads(r.read())
            break
        except (urllib.error.URLError, ConnectionError) as e:
            if time.time() - t0 > SERVER_WAIT:
                raise SystemExit('model server unavailable: %s' % e)
            time.sleep(10)
    msg = resp['choices'][0]['message']
    return msg.get('content') or '', msg.get('reasoning_content') or '', resp.get('usage', {}), \
        resp.get('timings', {}), time.time() - t0


def extract_c(text):
    text = re.sub(r'<think>.*?</think>', '', text, flags=re.S)
    if '</think>' in text:
        text = text.split('</think>')[-1]
    blocks = re.findall(r'```[cC]\s*\n(.*?)```', text, flags=re.S)
    blocks = [b for b in blocks if re.search(r'^\s*void\s+routine\s*\(', b, flags=re.M) and '...' not in b
              and len(b.strip().splitlines()) >= 3]
    return blocks[-1].strip() + '\n' if blocks else None


def log(entry):
    os.makedirs(M6, exist_ok=True)
    with open(LOG, 'a') as f:
        f.write(json.dumps(entry) + '\n')


def solve(bank, addr, tries):
    messages, outs, r = m6_prompt.build(bank, addr)
    base = messages
    last_code = last_msg = None
    work = os.path.join(M6, 'work', 'b%d_%04x' % (bank, addr))
    os.makedirs(work, exist_ok=True)
    total = {'prompt': 0, 'completion': 0, 'seconds': 0.0}
    for t in range(tries):
        content, reasoning, usage, timings, secs = ask(messages, TEMPS[min(t, len(TEMPS) - 1)])
        total['prompt'] += usage.get('prompt_tokens', 0)
        total['completion'] += usage.get('completion_tokens', 0)
        total['seconds'] += secs
        code = extract_c(content) or extract_c(reasoning)
        entry = {'time': time.strftime('%Y-%m-%d %H:%M:%S'), 'routine': r['name'], 'bank': bank,
                 'addr': '%04X' % addr, 'try': t + 1, 'prompt_tokens': usage.get('prompt_tokens'),
                 'completion_tokens': usage.get('completion_tokens'), 'cached_tokens':
                 timings.get('cache_n'), 'seconds': round(secs, 1), 'outputs': outs}
        if code is None:
            result, msg = 'noanswer', 'no ```c block with routine() in the answer'
        else:
            cand = os.path.join(work, 'try%d.c' % (t + 1))
            with open(cand, 'w', newline='\n') as f:
                f.write(code)
            rc, msg = m6_test.run_test(bank, addr, cand, outs or '-', work=work, show=m6_prompt.ram_reads(bank, addr),
                                       callees=r.get('callees'))
            result = {0: 'pass', 1: 'mismatch', 3: 'compile'}.get(rc, 'setup')
        entry.update({'result': result, 'message': msg[:400]})
        log(entry)
        print('  try %d: %s  (%s prompt + %s completion tokens, %.0f s)  %s' % (
            t + 1, result, usage.get('prompt_tokens'), usage.get('completion_tokens'), secs, msg[:160]))
        if result == 'pass':
            os.makedirs(PRIVATE, exist_ok=True)
            out = os.path.join(PRIVATE, '%s.c' % r['name'])
            with open(out, 'w', newline='\n') as f:
                f.write('/* %s: bank %d $%04X-$%04X. Private decompile (M6): passed the differential test, %s. '
                        'Never commit or ship. */\n' % (r['name'], bank, r['start'], r['end'], msg))
                f.write(code)
            return True, t + 1, total
        if result == 'setup':
            return False, t + 1, total
        # retry from a fresh conversation: the original prompt + the best code so far + its failure (a growing
        # chat made the model re-derive everything and run out of tokens while thinking)
        if code:
            last_code, last_msg = code, msg
        if last_code:
            retry = ('\n\nA previous answer:\n```c\n%s```\nfailed the differential test:\n%s\nFind the bug (check it '
                     'against the 6502 listing instruction by instruction around what the failure points at) and '
                     'write the corrected complete function.' % (last_code, last_msg))
        else:
            retry = '\n\nA previous attempt gave no usable ```c block (%s). Keep the reasoning short this time.' % msg
        messages = [base[0], {'role': 'user', 'content': base[1]['content'] + retry}]
    return False, tries, total


def safe_solve(bank, addr, tries):
    """solve() that logs an unexpected error and moves on instead of ending the batch."""
    try:
        return solve(bank, addr, tries)
    except (OSError, ValueError, KeyError, StopIteration, RuntimeError) as e:
        log({'time': time.strftime('%Y-%m-%d %H:%M:%S'), 'bank': bank, 'addr': '%04X' % addr, 'try': 0,
             'result': 'error', 'message': repr(e)[:400]})
        print('  error: %r' % e)
        return False, 0, {'prompt': 0, 'completion': 0, 'seconds': 0.0}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--routines', type=int, default=5)
    ap.add_argument('--list', default=os.path.join(M6, 'leaf40.txt'))
    ap.add_argument('--only')
    ap.add_argument('--tries', type=int, default=3)
    ap.add_argument('--stub-callees', action='store_true',
                    help='with --callgraph: do not wait for callees; those without passing C run as original code')
    ap.add_argument('--callgraph', action='store_true',
                    help='after the list: routines whose callees all have passing C, re-ranked after every pass')
    a = ap.parse_args()
    if a.callgraph and not a.only:
        return callgraph_mode(a)
    if a.only:
        todo = [(int(x.split(':')[0]), int(x.split(':')[1], 16)) for x in a.only.split(',')]
    else:
        todo = []
        for line in open(a.list):
            f = line.split()
            if len(f) >= 2:
                todo.append((int(f[0], 16), int(f[1], 16)))
    done = set(os.path.splitext(n)[0] for n in os.listdir(PRIVATE)) if os.path.isdir(PRIVATE) else set()
    results = []
    for bank, addr in todo:
        if len(results) >= a.routines:
            break
        name = next((r['name'] for r in m6_prompt.m6_pick.routines() if r['bank'] == bank and r['start'] == addr),
                    None)
        if name in done:
            continue
        if not m6_test.corpus_files(bank, addr, [os.path.join(M6, 'corpus', '2p'), os.path.join(M6, 'corpus', '1p')]):
            continue
        print('%s (bank %d $%04X)' % (name, bank, addr))
        ok, n, tot = safe_solve(bank, addr, a.tries)
        results.append((name, ok, n, tot))
    summary(results)


def callgraph_mode(a):
    """Non-leaf routines whose callees all pass, one at a time (the ready set grows as routines pass)."""
    tried, results = set(), []
    dirs = [os.path.join(M6, 'corpus', '2p'), os.path.join(M6, 'corpus', '1p')]
    while len(results) < a.routines:
        passing = set(os.path.splitext(n)[0] for n in os.listdir(PRIVATE)) if os.path.isdir(PRIVATE) else set()
        ready = [(c, r, names) for c, r, names in m6_prompt.m6_pick.callgraph_ready(passing, stubs=a.stub_callees)
                 if r['name'] not in tried and m6_test.corpus_files(r['bank'], r['start'], dirs)]
        if not ready:
            print('no more routines whose callees all pass (and that have a corpus)')
            break
        c, r, names = ready[0]
        tried.add(r['name'])
        print('%s (bank %d $%04X) calls %s' % (r['name'], r['bank'], r['start'], ', '.join(names)))
        ok, n, tot = safe_solve(r['bank'], r['start'], a.tries)
        results.append((r['name'], ok, n, tot))
    summary(results)


def summary(results):
    passed = sum(1 for r in results if r[1])
    print('\n%d/%d passed' % (passed, len(results)))
    for name, ok, n, tot in results:
        print('  %-12s %-4s tries %d  prompt %6d  completion %6d  %6.0f s' % (
            name, 'PASS' if ok else 'fail', n, tot['prompt'], tot['completion'], tot['seconds']))


if __name__ == '__main__':
    main()
