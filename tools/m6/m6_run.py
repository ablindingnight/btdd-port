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
MAX_TOKENS = 16000
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
    blocks = re.findall(r'```(?:c|C)?\s*\n(.*?)```', text, flags=re.S)
    blocks = [b for b in blocks if 'routine' in b]
    return blocks[-1].strip() + '\n' if blocks else None


def log(entry):
    os.makedirs(M6, exist_ok=True)
    with open(LOG, 'a') as f:
        f.write(json.dumps(entry) + '\n')


def solve(bank, addr, tries):
    messages, outs, r = m6_prompt.build(bank, addr)
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
            rc, msg = m6_test.run_test(bank, addr, cand, outs or '-', work=work)
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
        messages = messages + [
            {'role': 'assistant', 'content': '```c\n%s```' % code if code else content[-2000:]},
            {'role': 'user', 'content': 'The differential test failed:\n%s\nFix the function and answer with the '
                                        'complete corrected ```c block.' % msg}]
    return False, tries, total


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--routines', type=int, default=5)
    ap.add_argument('--list', default=os.path.join(M6, 'leaf40.txt'))
    ap.add_argument('--only')
    ap.add_argument('--tries', type=int, default=3)
    a = ap.parse_args()
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
        print('%s (bank %d $%04X)' % (name, bank, addr))
        ok, n, tot = solve(bank, addr, a.tries)
        results.append((name, ok, n, tot))
    passed = sum(1 for r in results if r[1])
    print('\n%d/%d passed' % (passed, len(results)))
    for name, ok, n, tot in results:
        print('  %-12s %-4s tries %d  prompt %6d  completion %6d  %6.0f s' % (
            name, 'PASS' if ok else 'fail', n, tot['prompt'], tot['completion'], tot['seconds']))


if __name__ == '__main__':
    main()
