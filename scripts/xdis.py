#!/usr/bin/env python3
"""xdis.py <va_hex> [count] [--bytes N]: disassemble assets/default.xbe at a guest VA (capstone, .local/venv).
Sections map raw = va - sec.va + sec.raw (analysis/xbe.json from xbeinfo.py)."""
import json, sys, os, struct
import capstone
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
x = json.load(open(os.path.join(root, 'analysis', 'xbe.json')))
d = open(os.path.join(root, 'assets', 'default.xbe'), 'rb').read()
def raw(va):
    for s in x['sections']:
        if s['va'] <= va < s['va'] + s['rsize']:
            return va - s['va'] + s['raw']
    return None
def read(va, n):
    o = raw(va); return d[o:o + n] if o is not None else b''
def u32(va): return struct.unpack('<I', read(va, 4))[0]
if __name__ == '__main__':
    va = int(sys.argv[1], 16); count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.syntax = capstone.CS_OPT_SYNTAX_INTEL
    for i, ins in enumerate(md.disasm(read(va, count * 15), va)):
        if i >= count: break
        print(f'{ins.address:08X}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}')
