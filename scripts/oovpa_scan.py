#!/usr/bin/env python3
"""oovpa_scan.py <xbsdb src/OOVPADatabase> <default.xbe> [--names a,b] [--json out]

An independent reader of XbSymbolDatabase's signature data (OOVPA tables, MIT licence; a local clone, never
vendored): parses the .inl patterns and the *_OOVPA.c registrations, picks for each symbol the newest signature not
newer than the XBE's library build, and scans the XBE's sections. XREF entries are checked against symbols found
in earlier passes (rel32 or absolute); an xref no signature produces (data derived by the library) is a wildcard.
A cross-check for xbe_symbols, like Breakdown's reconcile.md: every address it gives still needs checking by hand."""
import json, os, re, struct, sys

db, xbe_path = sys.argv[1], sys.argv[2]
want = set(sys.argv[sys.argv.index('--names') + 1].split(',')) if '--names' in sys.argv else None
d = open(xbe_path, 'rb').read()
u32 = lambda o: struct.unpack_from('<I', d, o)[0]
base = u32(0x104)
secs = []
for i in range(u32(0x11C)):
    o = u32(0x120) - base + i * 0x38
    fl, va, vs, ra, rs, na = struct.unpack_from('<6I', d, o)
    no = na - base
    secs.append((d[no:d.index(b'\0', no)].decode(), va, d[ra:ra + rs]))
libs = {}
lo = u32(0x164) - base
for i in range(u32(0x160)):
    o = lo + i * 16
    libs[d[o:o + 8].split(b'\0')[0].decode()] = struct.unpack_from('<H', d, o + 12)[0]
# XNet ships inside XONLINES in builds that list no XNETS library (MechAssault, 4831)
if 'XNETS' not in libs and 'XONLINES' in libs: libs['XNETS'] = libs['XONLINES']

# which sections each database scans (the XDK puts each library in its own section; XAPI and the CRT live in .text)
LIBS = {'D3D8': ('D3D8', ['D3D', 'D3DX']), 'DSound': ('DSOUND', ['DSOUND']), 'Xapi': ('XAPILIB', ['.text', 'XPP']),
        'XGraphic': ('XGRAPHC', ['XGRPH']), 'XNet': ('XNETS', ['XNET']), 'XOnline': ('XONLINES', ['XONLINE', 'XNET'])}

num = lambda s: int(s, 0)
sigs = {}  # (name, version) -> (xrefs [(off, xrefname)], pairs [(off, byte)])
hdr = re.compile(r'^(OOVPA_SIG_HEADER_(?:NO_)?XREF(?:_DETECT)?|OOVPA_(?:NO_)?XREF)\(\s*(\w+)\s*,\s*(\d+)', re.M)
for lib in LIBS:
    folder = os.path.join(db, lib)
    if not os.path.isdir(folder): continue
    for f in os.listdir(folder):
        text = open(os.path.join(folder, f), encoding='latin1').read()
        heads = list(hdr.finditer(text))
        for k, m in enumerate(heads):
            body = text[m.end():heads[k + 1].start() if k + 1 < len(heads) else len(text)]
            end = min([i for i in (body.find('OOVPA_END'), body.find(');')) if i >= 0] or [len(body)])
            body = re.sub(r'//[^\n]*', '', body[:end])
            xr = [(num(a), b) for a, b in re.findall(r'XREF_ENTRY\(\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(\w+)\s*\)', body)]
            pr = [(num(a), num(b)) for a, b in re.findall(r'\{\s*(0x[0-9A-Fa-f]+|\d+)\s*,\s*(0x[0-9A-Fa-f]+|\d+)\s*\}', body)]
            sigs[(m.group(2), int(m.group(3)))] = (lib, xr, pr)

# registrations: symbol -> versions
reg = {}
for lib in LIBS:
    p = os.path.join(db, lib + '_OOVPA.c')
    if not os.path.exists(p): continue
    text = re.sub(r'//[^\n]*', '', open(p, encoding='latin1').read())
    for m in re.finditer(r'REGISTER_OOVPAS(?:_\w+)?\(\s*(?:\w+\s*,\s*)?SYM_(?:FUN|VAR|FUN_LTCG)\(\s*(\w+)(.*?)SYM_SIG\(([^)]*)\)', text, re.S):
        if 'LTCG' in m.group(0)[:60]: continue
        vers = [int(v) for v in re.findall(r'\d+', m.group(3))]
        reg.setdefault((lib, m.group(1)), set()).update(vers)

chosen = {}
for (lib, name), vers in reg.items():
    build = libs.get(LIBS[lib][0])
    if build is None: continue
    ok = [v for v in vers if v <= build and (name, v) in sigs]
    if ok: chosen[name] = (lib, max(ok))
produced = {'XREF_' + n for n in chosen}

def regex(pairs):
    pairs = sorted(pairs)
    out, pos = b'', 0
    for off, val in pairs:
        if off < pos: continue
        if off > pos: out += b'.{%d}' % (off - pos)
        out += re.escape(bytes([val])); pos = off + 1
    return re.compile(b'(?=' + out + b')', re.S), pairs[0][0] if pairs else 0

found, ambiguous = {}, {}
for _ in range(6):
    changed = False
    for name, (lib, ver) in sorted(chosen.items()):
        if name in found: continue
        _, xr, pr = sigs[(name, ver)]
        if not pr: continue
        if any(x in produced and x[5:] not in found for _, x in xr): continue
        rx, _ = regex(pr)
        hits = []
        for sname, sva, data in secs:
            if sname not in LIBS[lib][1]: continue
            for m in rx.finditer(data):
                a, ok = sva + m.start(), True
                for off, x in xr:
                    if x not in produced: continue
                    t = found[x[5:]]
                    o = m.start() + off
                    if o + 4 > len(data): ok = False; break
                    v = struct.unpack_from('<I', data, o)[0]
                    if v != t and (a + off + 4 + v) & 0xFFFFFFFF != t: ok = False; break
                if ok: hits.append(a)
        if len(hits) == 1: found[name] = hits[0]; changed = True
        elif hits: ambiguous[name] = hits
    if not changed: break

rows = {n: found.get(n) for n in chosen if want is None or n in want}
for n in sorted(rows):
    v = rows[n]
    print(f'{n:52} {"0x%06X" % v if v else ("ambiguous " + " ".join("%X" % h for h in ambiguous[n][:6]) if n in ambiguous else "-")}  ({chosen[n][0]} {chosen[n][1]})')
if '--json' in sys.argv:
    json.dump({n: v for n, v in found.items()}, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1, sort_keys=True)
