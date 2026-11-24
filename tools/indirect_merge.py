"""indirect_merge.py: merge the *-indirect.tsv files of btdd_headless --indirect / btdd_explore into one table.

  python tools/indirect_merge.py OUT.tsv IN_GLOB [IN_GLOB ...] [--sites 0:FDFD,0:FE20,...]
Output columns: kind, site_bank, site_pc, target_bank, target_pc, slot_jmp_target, count, sources (input
file stems). --sites prints, for each listed site (bank:pc in hex), the targets found or "no target seen".
Addresses only; still keep the files under ..\\Gamefiles.
"""
import glob, os, sys


def main():
    args = sys.argv[1:]
    sites = []
    if '--sites' in args:
        k = args.index('--sites')
        sites = [s.split(':') for s in args[k + 1].split(',')]
        del args[k:k + 2]
    out, pats = args[0], args[1:]
    rows = {}
    files = sorted({f for p in pats for f in glob.glob(p)})
    for f in files:
        stem = os.path.basename(f).replace('-indirect.tsv', '')
        for line in open(f):
            c = line.rstrip('\n').split('\t')
            if c[0] == 'kind' or len(c) < 7:
                continue
            key = tuple(c[:6])
            n, src = rows.get(key, (0, set()))
            src.add(stem)
            rows[key] = (n + int(c[6]), src)
    with open(out, 'w', newline='\n') as fo:
        fo.write('kind\tsite_bank\tsite_pc\ttarget_bank\ttarget_pc\tslot_jmp_target\tcount\tsources\n')
        for key in sorted(rows):
            n, src = rows[key]
            fo.write('\t'.join(key) + '\t%d\t%s\n' % (n, ','.join(sorted(src))))
    print('%d files, %d (site, target) pairs, %d sites' % (len(files), len(rows), len({k[:3] for k in rows})))
    for b, pc in sites:
        hits = [(k, rows[k][0]) for k in rows if k[1] == str(int(b, 16)) and k[2].upper() == '$' + pc.upper()]
        if not hits:
            print('  %s:$%s  no target seen' % (b, pc.upper()))
        for k, n in sorted(hits):
            print('  %s:$%s  %-7s -> %s:%s%s  x%d' % (b, pc.upper(), k[0], k[3], k[4],
                                                    (' (slot jmp %s)' % k[5]) if k[5] != '-' else '', n))


if __name__ == '__main__':
    main()
