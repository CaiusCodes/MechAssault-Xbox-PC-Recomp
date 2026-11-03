"""mechassault-recomp: draws res/mechassault.ico, the executable's icon (own art: a red targeting reticle on a dark disc
in a steel ring, not the game's logo). python scripts/icon/make_icon.py"""
import os
from PIL import Image, ImageDraw, ImageFilter

def draw(n):
    s = 4 * n  # drawn large, scaled down smooth
    im = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = s / 2
    def circle(r, **kw): d.ellipse([c - r, c - r, c + r, c + r], **kw)
    circle(s * 0.48, fill=(200, 202, 210, 255))             # steel ring
    circle(s * 0.43, fill=(22, 22, 26, 255))
    circle(s * 0.40, fill=(110, 14, 22, 255))               # dark red disc
    glow = Image.new('RGBA', (s, s), (0, 0, 0, 0))
    ImageDraw.Draw(glow).ellipse([c - s * 0.2, c - s * 0.2, c + s * 0.2, c + s * 0.2], fill=(230, 60, 66, 170))
    im = Image.alpha_composite(im, glow.filter(ImageFilter.GaussianBlur(s * 0.08)))
    d = ImageDraw.Draw(im)
    w = max(2, int(s * 0.022))
    d.ellipse([c - s * 0.25, c - s * 0.25, c + s * 0.25, c + s * 0.25], outline=(245, 245, 248, 255), width=w)
    for a, b in [((c, s * 0.10), (c, c - s * 0.09)), ((c, c + s * 0.09), (c, s * 0.90)),
                 ((s * 0.10, c), (c - s * 0.09, c)), ((c + s * 0.09, c), (s * 0.90, c))]:
        d.line([a, b], fill=(245, 245, 248, 255), width=w)
    d.ellipse([c - s * 0.03, c - s * 0.03, c + s * 0.03, c + s * 0.03], fill=(255, 90, 90, 255))
    return im.resize((n, n), Image.LANCZOS)

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sizes = [256, 64, 48, 32, 24, 16]
big = draw(256)
big.save(os.path.join(root, 'res', 'mechassault.ico'), sizes=[(n, n) for n in sizes],
         append_images=[draw(n) for n in sizes[1:]])
big.save(os.path.join(root, 'res', 'mechassault_icon_preview.png'))
