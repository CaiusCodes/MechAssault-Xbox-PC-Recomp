#!/usr/bin/env python3
"""vtable_audit.py: methods in the XBE's tables that have no recompiled function.

A call through one of them stops the game ("indirect call ... has no recompiled function"). The MechAssault 2 recomp
found virtual methods that nothing calls directly, missed by its function finder, on paths its first mission never
took (tanks, cameras). This looks at every run of code pointers in the data sections that is mostly function starts
(a vtable or a table of callbacks) and reports the entries that are not, when they sit on an instruction boundary
after a return or a jump, or open with push ebp: a method decoded as the tail of the function before it (an entry
right after a call is a return address, not a method).
Add each one to [functions] in config/game.toml. Needs capstone: .local/venv/Scripts/python.exe scripts/vtable_audit.py"""
import bisect, json, os, re, struct

import capstone

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
x = json.load(open(os.path.join(root, 'analysis', 'xbe.json')))
img = open(os.path.join(root, 'assets', 'default.xbe'), 'rb').read()
mapping = open(os.path.join(root, 'generated', 'game', 'x86_func_mapping.cpp')).read()
funcs = sorted({int(m, 16) for m in re.findall(r'\{0x([0-9A-F]+)u, sub_', mapping)})
starts = set(funcs)
# every XBE section is flagged executable: the recompiled range is the code, the rest is data
lo = int(re.search(r'X86CodeBase = 0x([0-9A-F]+)u', mapping).group(1), 16)
hi = int(re.search(r'X86CodeEnd = 0x([0-9A-F]+)u', mapping).group(1), 16)
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)


def read(va, n):
    for s in x['sections']:
        if s['va'] <= va < s['va'] + s['rsize']:
            o = s['raw'] + va - s['va']
            return img[o:o + n]
    return b''


runs = []
for s in x['sections']:
    if s['va'] < hi or not s['rsize']:
        continue
    data = img[s['raw']:s['raw'] + s['rsize']]
    run = []
    for off in range(0, len(data) - 3, 4):
        v = struct.unpack_from('<I', data, off)[0]
        if lo <= v < hi:
            run.append((s['va'] + off, v))
            continue
        if len(run) >= 2:
            runs.append(run)
        run = []

found = 0
for run in runs:
    n = sum(v in starts for _, v in run)
    if n < 2 or n < 0.6 * len(run):
        continue  # mostly not function starts: numbers that happen to fall in the code range
    for at, v in run:
        if v in starts:
            continue
        owner = funcs[bisect.bisect_right(funcs, v) - 1]
        prev = None
        for i in md.disasm(read(owner, v - owner + 16), owner):
            if i.address >= v:
                break
            prev = i
        if not prev or prev.address + prev.size != v:
            continue  # not on an instruction boundary
        if prev.mnemonic in ('ret', 'jmp', 'int3') or read(v, 1) == b'\x55':
            first = next(md.disasm(read(v, 16), v), None)
            print(f'{v:08X} in {owner:08X}, table entry at {at:08X}: after "{prev.mnemonic} {prev.op_str}", '
                  f'starts "{first.mnemonic} {first.op_str}"')
            found += 1
print(f'{len(funcs)} functions, {len(runs)} tables of code pointers, {found} methods without a function')
