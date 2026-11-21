"""Lists the functions in an address range that use given device offsets: as a load/store displacement
(`lwz rX,<off>(rY)`) or as an addi base (`addi rX,rY,<off>`, the start of an indexed array). For checking the
"model" offsets of FNC's device layout (docs/fnc/stage2.md) against the code that really uses them.

    python tools/offset_users.py 12636 12704 12720 [--low 82CD8000] [--high 82D10000] [--gen app/generated/default]
"""
import argparse
import collections
import glob
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument('offsets', nargs='+', type=int)
ap.add_argument('--low', default='82CD8000')
ap.add_argument('--high', default='82D10000')
ap.add_argument('--gen', default=os.path.join(ROOT, 'app', 'generated', 'default'))
a = ap.parse_args()
low, high = int(a.low, 16), int(a.high, 16)
wanted = set(a.offsets)

func_re = re.compile(r'^DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)')
mem_re = re.compile(r'^\t// ([a-z]+) r\d+,(-?\d+)\(r\d+\)$')
addi_re = re.compile(r'^\t// addi r\d+,r\d+,(-?\d+)$')
users = collections.defaultdict(lambda: collections.defaultdict(set))
for path in glob.glob(os.path.join(a.gen, '*_recomp.*.cpp')):
    cur = None
    for line in open(path, errors='replace'):
        m = func_re.match(line)
        if m:
            addr = int(m.group(1), 16)
            cur = m.group(1) if low <= addr < high else None
            continue
        if not cur:
            continue
        m = mem_re.match(line)
        if m and int(m.group(2)) in wanted:
            users[int(m.group(2))][cur].add(m.group(1))
            continue
        m = addi_re.match(line)
        if m and int(m.group(1)) in wanted:
            users[int(m.group(1))][cur].add('addi')

for off in a.offsets:
    funcs = users.get(off, {})
    listed = ', '.join(f'sub_{f}({"/".join(sorted(ops))})' for f, ops in sorted(funcs.items())[:8])
    print(f'{off}: {len(funcs)} functions{": " + listed if listed else ""}')
