"""Finds the guest functions that build PM4 type-3 packet headers for given opcodes (FNC has no reference game with
named Direct3D functions, so the draws, shader loads and resolves are found by the packets they write).

A type-3 header is 0xC0000000 | (count - 1) << 16 | opcode << 8. The generated code keeps each PPC instruction as a
comment; this looks for `lis rX,-163xx` (the 0xC0nn high half) followed within a few instructions by
`ori rY,rX,<opcode << 8 | predicate>` or `addi` of the same low half, and reports the function and the packet.

    python tools/pm4_writers.py app/generated/default [--ops 22,36,27,2B,2D,3D]
"""
import argparse
import collections
import glob
import os
import re

OP_NAMES = {0x22: 'DRAW_INDX', 0x36: 'DRAW_INDX_2', 0x27: 'IM_LOAD', 0x2B: 'IM_LOAD_IMMEDIATE', 0x2D: 'SET_CONSTANT',
            0x3D: 'MEM_WRITE', 0x3C: 'WAIT_REG_MEM', 0x46: 'EVENT_WRITE', 0x58: 'EVENT_WRITE_SHD',
            0x5B: 'EVENT_WRITE_EXT', 0x48: 'ME_INIT', 0x10: 'NOP', 0x3F: 'INDIRECT_BUFFER', 0x2F: 'SET_CONSTANT2',
            0x4C: 'INVALIDATE_STATE', 0x26: 'VIZ_QUERY', 0x21: 'SET_SHADER_CONSTANTS'}

ap = argparse.ArgumentParser()
ap.add_argument('gen')
ap.add_argument('--ops', default='22,36,27,2B')
a = ap.parse_args()
want = {int(x, 16) for x in a.ops.split(',')}

func_re = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)')
lis_re = re.compile(r'^\t// lis (r\d+),(-?\d+)$')
low_re = re.compile(r'^\t// (?:ori|addi) (r\d+),(r\d+),(-?\d+)$')
hits = collections.defaultdict(list)
for path in sorted(glob.glob(os.path.join(a.gen, '*_recomp.*.cpp'))):
    cur, highs = None, {}
    for line in open(path, errors='replace'):
        m = func_re.match(line)
        if m:
            cur, highs = m.group(1), {}
            continue
        m = lis_re.match(line)
        if m:
            hi = int(m.group(2)) & 0xFFFF
            if hi >> 14 == 3:  # 0xC000-0xFFFF: a type-3 header's high half
                highs[m.group(1)] = (hi, 0)
            continue
        m = low_re.match(line)
        if m and m.group(2) in highs:
            hi, _ = highs[m.group(2)]
            lo = int(m.group(3)) & 0xFFFF
            value = (hi << 16) + (lo if 'ori' in line else int(m.group(3))) & 0xFFFFFFFF
            if value >> 30 == 3:
                op = (value >> 8) & 0x7F
                if op in want:
                    hits[cur].append(((value >> 16) & 0x3FFF, op))
        # Any other instruction keeps the tracked highs: a header may be built a few instructions later.

for func, packets in sorted(hits.items()):
    kinds = collections.Counter(f'{OP_NAMES.get(op, hex(op))}[{n + 1}]' for n, op in packets)
    print(func, ', '.join(f'{k} x{c}' for k, c in sorted(kinds.items())))
