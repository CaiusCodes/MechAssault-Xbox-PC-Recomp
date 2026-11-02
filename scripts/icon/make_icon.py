# Draws res/breakdown.ico, the "breaking comb" icon: honeycomb.py's breaking() drawn for each size (a simpler drawing
# at 16-20 and 24-40 px), rendered by Chromium through Playwright, packed as 32-bit DIBs with a PNG at 256 px.
#   python scripts/icon/make_icon.py        (needs Python 3 with Pillow, Node with Playwright)
import io, os, re, struct, subprocess, sys, tempfile
from PIL import Image
here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)
import honeycomb as hc

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
def detail(s): return 256 if s >= 48 else (32 if s >= 24 else 16)

tmp = tempfile.mkdtemp()
parts = []
for s in SIZES:
    i = f"z{s}"
    body = re.sub(r'<circle cx="112" cy="140" r="110"[^>]*/>', '', hc.breaking(i, detail(s), False))  # no halo
    parts.append(f'<div id="s{s}" style="width:{s}px;height:{s}px"><svg width="{s}" height="{s}" '
                 f'viewBox="0 0 256 256">{hc.defs(i)}<g class="art">{body}</g></svg></div>')
fit = '''<script>document.querySelectorAll('svg').forEach(function(s){var b=s.querySelector('.art').getBBox();
var m=Math.max(b.width,b.height)*1.1;s.setAttribute('viewBox',(b.x+b.width/2-m/2)+' '+(b.y+b.height/2-m/2)+' '+m+' '+m);});</script>'''
open(os.path.join(tmp, 'icons.html'), 'w').write('<html><body style="margin:0">' + ''.join(parts) + fit + '</body></html>')
shoot = f'''const {{ chromium }} = require('playwright');
(async () => {{ const b = await chromium.launch(); const p = await b.newPage();
await p.goto('file://{tmp}/icons.html'); await p.waitForTimeout(500);
for (const s of {SIZES}) await (await p.$('#s'+s)).screenshot({{path:'{tmp}/c'+s+'.png', omitBackground:true}});
await b.close(); }})();'''
open(os.path.join(tmp, 'shoot.js'), 'w').write(shoot)
subprocess.run(['node', os.path.join(tmp, 'shoot.js')], check=True)

entries = []
for s in SIZES:
    im = Image.open(os.path.join(tmp, f'c{s}.png')).convert('RGBA')
    if s >= 256:
        b = io.BytesIO(); im.save(b, 'PNG', optimize=True); data = b.getvalue()
    else:
        px = im.transpose(Image.FLIP_TOP_BOTTOM).tobytes('raw', 'BGRA')
        row = ((s + 31) // 32) * 4
        mask = bytearray()
        for y in range(s - 1, -1, -1):
            r = bytearray(row)
            for x in range(s):
                if im.getpixel((x, y))[3] == 0: r[x // 8] |= 0x80 >> (x % 8)
            mask += r
        data = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, len(px) + len(mask), 0, 0, 0, 0) + px + bytes(mask)
    entries.append((s, data))
head = struct.pack('<HHH', 0, 1, len(entries)); off = 6 + 16 * len(entries); body = b''
for s, d in entries:
    head += struct.pack('<BBBBHHII', s % 256, s % 256, 0, 0, 1, 32, len(d), off); off += len(d); body += d
out = os.path.join(here, '..', '..', 'res', 'breakdown.ico')
open(out, 'wb').write(head + body)
print('wrote', os.path.normpath(out))
