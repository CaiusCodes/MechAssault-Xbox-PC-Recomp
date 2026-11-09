"""mechassault-recomp: translate_config.py <2002 mechassault.toml> <out>: the hand-checked symbols and hooks moved to
Rev 1 by code matching (xbe_match.py), each line keeping its 2002 address in a comment. Fixed: addresses given in
MANUAL (found by hand, see docs/REV1.md). Prints what it could not place; those lines are left at 0 and flagged."""
import contextlib, io, re, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
with contextlib.redirect_stdout(io.StringIO()):
    import xbe_match as m

MANUAL = {
    0x8B0A3: 0x8D6D3,   # campaign hook: after the profile load call in the campaign menu (0x8D690), same code
    0x2517AE: 0x27186E, # WSAGetLastError: the XNet stub that jumps to XAPI's GetLastError (0x16EEE -> 0x17ACE)
}
XNET_SHIFT = 0x200C0  # the XNet library moved as a block (every stub found agrees)

src, dst = sys.argv[1], sys.argv[2]
out, missing = [], []
for line in open(src, encoding="utf-8"):
    line = line.rstrip("\n")
    mm = re.match(r"(\s*(?:\w+\s*=|at\s*=)\s*)(0x[0-9A-Fa-f]+)(.*)$", line)
    if not mm or line.lstrip().startswith("#"):
        out.append(line); continue
    old = int(mm.group(2), 16)
    if old == 0:
        out.append(line); continue
    new = MANUAL.get(old, "auto")
    how = "by hand"
    if new == "auto":
        new, how = m.locate(old)
        if new is None and 0x251000 <= old < 0x258000:
            cand = old + XNET_SHIFT
            if m.on[old - m.olo:old - m.olo + 11] == m.nn[cand - m.nlo:cand - m.nlo + 11]: new, how = cand, "XNet block shift"
    if new is None:
        missing.append((line.strip(), how))
        out.append(f"{mm.group(1)}0{mm.group(3)}  # REV1: not placed yet (2002: {old:#X})")
        continue
    rest = mm.group(3)
    note = f"2002: {old:#X}"
    rest = (rest + "  # " + note) if "#" not in rest else rest.replace("#", f"# {note};", 1)
    out.append(f"{mm.group(1)}{new:#X}{rest}".replace("0X", "0x"))
open(dst, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
print(f"{len(missing)} not placed:")
for l, h in missing: print("  ", l, "--", h)
