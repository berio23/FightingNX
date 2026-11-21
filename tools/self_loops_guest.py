"""Prints the tight self-loops of given recompiled functions (a label whose own `goto` follows within a few lines), to
tell a guest halt (the loop re-tests a register it never reloads, as the PowerPC does) from a polling loop over memory
(a load inside the loop, which the compiler must not hoist). tools/check_switch_build.py flags both as branch-to-self.

    python tools/self_loops_guest.py sub_82B2CF88 sub_82C87E48 [--gen app/generated/default] [--max 12]
"""
import argparse
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument('funcs', nargs='+')
ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated', 'default'))
ap.add_argument('--max', type=int, default=12)
a = ap.parse_args()

for name in a.funcs:
    text = None
    for path in glob.glob(os.path.join(a.gen, '*_recomp.*.cpp')):
        src = open(path, errors='replace').read()
        i = src.find(f'DEFINE_REX_FUNC({name})')
        if i >= 0:
            j = src.find('\nDEFINE_REX_FUNC(', i + 1)
            text = src[i:j if j > 0 else len(src)]
            break
    if text is None:
        print(f'{name}: not found')
        continue
    lines = text.splitlines()
    found = False
    for n, line in enumerate(lines):
        m = re.match(r'^(loc_[0-9A-F]+):', line)
        if not m:
            continue
        for k in range(n + 1, min(n + a.max, len(lines))):
            if f'goto {m.group(1)};' in lines[k]:
                body = lines[n:k + 1]
                loads = sum('REX_LOAD' in b or 'PPC_LOAD' in b for b in body)
                kind = 'polling loop (re-reads memory)' if loads else 'guest halt (no load in the loop)'
                print(f'== {name} {m.group(1)}: {kind}')
                print('\n'.join(body))
                found = True
                break
    if not found:
        print(f'{name}: no tight self-loop within {a.max} lines')
