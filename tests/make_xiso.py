#!/usr/bin/env python3
"""Test helper: builds an Xbox DVD file system image (xiso) from a folder, or a whole-disc layout with
--full (game partition at 0x18300000, written sparse). Only for unit tests of the import code; it does not
reproduce a real disc's layout or order."""
import os, struct, sys

SECTOR = 2048
FULL = 0x18300000

def build(src, out, full):
    base = FULL if full else 0
    data = []          # (sector, bytes or path)
    next_sector = [33] # after the volume descriptor at 32

    def alloc(nbytes):
        s = next_sector[0]
        next_sector[0] += max(1, (nbytes + SECTOR - 1) // SECTOR)
        return s

    def table(path):
        names = sorted(os.listdir(path), key=lambda n: n.upper())
        entries = []
        for n in names:
            p = os.path.join(path, n)
            if os.path.isdir(p):
                sec, size = table(p)
                entries.append((n, sec, size, 0x10))
            else:
                size = os.path.getsize(p)
                sec = alloc(size)
                data.append((sec, p))
                entries.append((n, sec, size, 0x20))
        # a chain through the right links, padded so no entry crosses a sector
        blob = bytearray()
        offs = []
        for i, (n, sec, size, attr) in enumerate(entries):
            ln = 14 + len(n)
            ln4 = (ln + 3) & ~3
            if len(blob) // SECTOR != (len(blob) + ln - 1) // SECTOR:
                blob += b'\xff' * (SECTOR - len(blob) % SECTOR)
            offs.append(len(blob))
            blob += b'\0' * ln4
        for i, (n, sec, size, attr) in enumerate(entries):
            o = offs[i]
            right = offs[i + 1] // 4 if i + 1 < len(entries) else 0
            e = struct.pack('<HHIIBB', 0, right, sec, size, attr, len(n)) + n.encode()
            blob[o:o + len(e)] = e
        if not entries:
            return 0, 0
        size = len(blob)
        blob += b'\xff' * ((-len(blob)) % SECTOR)
        sec = alloc(len(blob))
        data.append((sec, bytes(blob)))
        return sec, size

    root_sec, root_size = table(src)
    with open(out, 'wb') as f:
        vol = bytearray(SECTOR)
        magic = b'MICROSOFT*XBOX*MEDIA'
        vol[0:20] = magic
        struct.pack_into('<IIQ', vol, 0x14, root_sec, root_size, 0x01C4000000000000)
        vol[0x7EC:0x7EC + 20] = magic
        f.seek(base + 32 * SECTOR); f.write(vol)
        for sec, d in data:
            f.seek(base + sec * SECTOR)
            if isinstance(d, bytes): f.write(d)
            else:
                with open(d, 'rb') as g: f.write(g.read())
        f.truncate(base + next_sector[0] * SECTOR)

if __name__ == '__main__':
    full = '--full' in sys.argv
    a = [x for x in sys.argv[1:] if x != '--full']
    build(a[0], a[1], full)
