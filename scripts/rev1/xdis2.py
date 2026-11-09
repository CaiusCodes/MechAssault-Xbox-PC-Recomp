"""mechassault-recomp: xdis2.py old|new <va_hex> [count]: disassemble the 2002 (old) or Rev 1 (new) default.xbe at a
guest address, side by side work for moving the port's addresses to Rev 1. .local\\venv\\Scripts\\python.exe (capstone).
xdis2.py refs old|new <va_hex>: instructions that use the address as an immediate or displacement."""
import os, struct, sys
import capstone

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
XBE = {"old": "game_files/MechAssault (USA, Europe) (En,Fr,De)/default.xbe",
       "new": "game_files/MechAssault (USA) (En,Fr,De) (Rev 1)/default.xbe"}


def image(which):
    x = open(os.path.join(ROOT, XBE[which]), "rb").read()
    r = lambda o: struct.unpack_from("<I", x, o)[0]
    base, n, h = r(0x104), r(0x11C), r(0x120) - r(0x104)
    secs = []
    for i in range(n):
        o = h + i * 0x38
        secs.append((r(o + 4), r(o + 8), r(o + 12), r(o + 16), r(o) & 4))
    img = bytearray(max(va + vs for va, vs, *_ in secs))
    for va, vs, raw, rs, _ in secs:
        img[va:va + rs] = x[raw:raw + rs]
    return bytes(img), secs


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
md.syntax = capstone.CS_OPT_SYNTAX_INTEL


def dis(which, va, count=40):
    img, _ = image(which)
    for i, ins in enumerate(md.disasm(img[va:va + count * 15], va)):
        if i >= count: break
        print(f"{ins.address:08X}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")


def refs(which, target):
    img, secs = image(which)
    pat = struct.pack("<I", target)
    for va, vs, raw, rs, ex in secs:
        if not ex: continue
        i = img.find(pat, va, va + rs)
        while i >= 0:
            # back up a few bytes to an instruction that holds the address
            for back in range(1, 8):
                ins = next(md.disasm(img[i - back:i + 8], i - back), None)
                if ins and ins.address + ins.size >= i + 4 and ins.address <= i:
                    print(f"{ins.address:08X}  {ins.mnemonic} {ins.op_str}")
                    break
            i = img.find(pat, i + 1, va + rs)


if __name__ == "__main__":
    if sys.argv[1] == "refs":
        refs(sys.argv[2], int(sys.argv[3], 16))
    else:
        dis(sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3]) if len(sys.argv) > 3 else 40)
