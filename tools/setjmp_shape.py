"""Finds the guest CRT's setjmp/longjmp by shape in ReXGlue's generated code (the PPC listing is in its comments).

setjmp: many std/stfd/vector stores (stvx/stvx128/stvlx128...) whose base register is r3, and no matching loads.
longjmp: the mirror image (ld/lfd/lvx/lvx128... from r3, then mtlr). Prints the strongest candidates of each kind.

    python tools/setjmp_shape.py <generated dir> [top N]
"""
import glob, os, re, sys
from collections import defaultdict

gen = sys.argv[1]
top = int(sys.argv[2]) if len(sys.argv) > 2 else 5
func_re = re.compile(r'^(?:PPC_FUNC_IMPL|REX_FUNC_IMPL|.*__imp__)\(?\s*(?:__imp__)?(sub_[0-9A-F]{8})')
ins_re = re.compile(r'^\s*//\s+([a-z0-9.]+)\s+(.*)$')
stats = defaultdict(lambda: defaultdict(int))
for path in glob.glob(os.path.join(gen, '*.cpp')):
    cur = None
    for line in open(path, errors='replace'):
        m = re.match(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL)\((?:__imp__)?(sub_[0-9A-F]{8})', line)
        if m:
            cur = m.group(1)
            continue
        if not cur:
            continue
        m = ins_re.match(line)
        if not m:
            continue
        op, args = m.groups()
        r3_mem = '(r3)' in args
        r3_idx = bool(re.search(r',r3$|,r3,|r0,r3', args))
        s = stats[cur]
        if op in ('std', 'stw') and r3_mem: s['gpr_st'] += 1
        elif op == 'stfd' and r3_mem: s['fpr_st'] += 1
        elif op.startswith('stv') and r3_idx: s['vec_st'] += 1
        elif op in ('ld', 'lwz') and r3_mem: s['gpr_ld'] += 1
        elif op == 'lfd' and r3_mem: s['fpr_ld'] += 1
        elif op.startswith('lv') and r3_idx: s['vec_ld'] += 1
        elif op == 'mtlr': s['mtlr'] += 1
        s['n'] += 1

def score(s, kind):
    a, b = ('st', 'ld') if kind == 'setjmp' else ('ld', 'st')
    return s['gpr_' + a] + s['fpr_' + a] + s['vec_' + a] - (s['gpr_' + b] + s['fpr_' + b] + s['vec_' + b])

for kind in ('setjmp', 'longjmp'):
    print(f'== {kind} candidates')
    for f, s in sorted(stats.items(), key=lambda kv: -score(kv[1], kind))[:top]:
        print(f'  {f}: {dict(s)}')
