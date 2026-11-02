#!/usr/bin/env python3
"""Independent XBE header parser (cross-check for xbe_symbols). Usage: xbeinfo.py default.xbe [--json out]"""
import struct, sys, json, hashlib
p = sys.argv[1]; d = open(p, 'rb').read()
u32 = lambda o: struct.unpack_from('<I', d, o)[0]
assert d[:4] == b'XBEH'
base = u32(0x104)
def off(va):
    # mechassault-recomp: headers map raw = va - base; sections by their own raw offset (MechAssault's do not match)
    n_, sh_ = u32(0x11C), u32(0x120) - base
    for i_ in range(n_):
        fl_, sva, vs_, ra_, rs_ = struct.unpack_from('<5I', d, sh_ + i_ * 0x38)
        if sva <= va < sva + rs_: return va - sva + ra_
    return va - base
info = {'sha256': hashlib.sha256(d).hexdigest(), 'sha1': hashlib.sha1(d).hexdigest(), 'size': len(d)}
info['base'] = base; info['headers_size'] = u32(0x108); info['image_size'] = u32(0x10C)
info['timedate'] = u32(0x114)
ep_raw, th_raw = u32(0x128), u32(0x158)
for kind, ke, kt in (('retail', 0xA8FC57AB, 0x5B6D40B6), ('debug', 0x94859D4B, 0xEFB1F152)):
    ep, th = ep_raw ^ ke, th_raw ^ kt
    if base <= ep < base + info['image_size']:
        info['key'] = kind; info['entry'] = ep; info['kernel_thunk'] = th; break
info['init_flags'] = u32(0x124); info['tls'] = u32(0x12C)
info['stack_commit'] = u32(0x130); info['heap_reserve'] = u32(0x134); info['heap_commit'] = u32(0x138)
info['pe_base'] = u32(0x13C); info['pe_size'] = u32(0x140); info['pe_timedate'] = u32(0x148)
def cstr(va):
    o = off(va); e = d.index(b'\0', o); return d[o:e].decode('latin1')
def wstr(o, n):
    return d[o:o+2*n].decode('utf-16le').split('\0')[0]
info['debug_path'] = cstr(u32(0x14C)) if u32(0x14C) else ''
c = off(u32(0x118))
info['cert'] = {'title_id': u32(c+8), 'title_name': wstr(c+0xC, 40), 'allowed_media': u32(c+0x9C),
                'region': u32(c+0xA0), 'ratings': u32(c+0xA4), 'disk': u32(c+0xA8), 'version': u32(c+0xAC),
                'timedate': u32(c+4)}
n, sh = u32(0x11C), off(u32(0x120))
secs = []
for i in range(n):
    o = sh + i*0x38
    fl, va, vs, ra, rs, na = struct.unpack_from('<6I', d, o)
    raw = d[ra:ra+rs]
    secs.append({'name': cstr(na), 'flags': fl, 'va': va, 'vsize': vs, 'raw': ra, 'rsize': rs,
                 'writable': bool(fl & 1), 'preload': bool(fl & 2), 'exec': bool(fl & 4), 'inserted': bool(fl & 8),
                 'headpage_ro': bool(fl & 16), 'tailpage_ro': bool(fl & 32), 'sha256': hashlib.sha256(raw).hexdigest()})
info['sections'] = secs
libs = []
nl, lo = u32(0x160), off(u32(0x164)) if u32(0x164) else 0
for i in range(nl):
    o = lo + i*16
    nm = d[o:o+8].split(b'\0')[0].decode(); mj, mn, bd, fl = struct.unpack_from('<4H', d, o+8)
    libs.append({'name': nm, 'version': f'{mj}.{mn}.{bd}', 'build': bd, 'qfe': fl & 0x1FFF, 'approved': (fl >> 13) & 3, 'debug': bool(fl & 0x8000)})
info['libraries'] = libs
if info.get('kernel_thunk'):
    t = off(info['kernel_thunk']); imps = []
    while True:
        v = u32(t); t += 4
        if not v: break
        imps.append(v & 0x7FFFFFFF)
    info['kernel_imports'] = imps
if info['tls']:
    o = off(info['tls']); s, e, idx, cb, zf, ch = struct.unpack_from('<6I', d, o)
    info['tls_dir'] = {'data_start': s, 'data_end': e, 'index_addr': idx, 'callbacks': cb, 'zero_fill': zf, 'characteristics': ch}
if '--json' in sys.argv: json.dump(info, open(sys.argv[sys.argv.index('--json')+1], 'w'), indent=1)
h = lambda v: f'0x{v:08X}'
print('sha256', info['sha256']); print('sha1  ', info['sha1'])
ct = info['cert']
print(f"title {ct['title_name']!r} id {h(ct['title_id'])} ({chr(ct['title_id']>>24)}{chr((ct['title_id']>>16)&255)}-{ct['title_id']&0xFFFF:03d}) region {h(ct['region'])} version {h(ct['version'])} media {h(ct['allowed_media'])} disk {ct['disk']}")
print('key', info.get('key'), 'base', h(base), 'entry', h(info['entry']), 'thunk', h(info['kernel_thunk']), 'image size', h(info['image_size']), 'init flags', h(info['init_flags']))
print('stack', h(info['stack_commit']), 'heap', h(info['heap_reserve']), h(info['heap_commit']), 'debug path', info['debug_path'])
print('tls', info.get('tls_dir'))
for s in secs:
    f = ''.join(c for c, b in (('W', s['writable']), ('P', s['preload']), ('X', s['exec']), ('I', s['inserted'])) if b)
    print(f"  {s['name']:10} va {h(s['va'])} vsize {h(s['vsize'])} raw {h(s['raw'])} rsize {h(s['rsize'])} {f}")
for l in libs: print(f"  lib {l['name']:9} {l['version']} qfe {l['qfe']} approved {l['approved']}{' DEBUG' if l['debug'] else ''}")
print('kernel imports', len(info.get('kernel_imports', [])))
