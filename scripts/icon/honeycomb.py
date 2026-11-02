# The honeycomb drawings of the port's icon (original artwork, after the cover's blood-red cells); make_icon.py draws the icon from breaking().
import math, sys
S3=math.sqrt(3)
def hexpts(cx,cy,r):
    return " ".join(f"{cx+r*math.cos(math.radians(a)):.2f},{cy+r*math.sin(math.radians(a)):.2f}" for a in range(0,360,60))
def grid(cx,cy,r,q,s):
    # flat-top axial: x=1.5r q, y=S3 r (s + q/2)
    return cx+1.5*r*q, cy+S3*r*(s+q/2)

uid=[0]
def defs(i):
    return f'''<defs>
<radialGradient id="hot{i}" cx="50%" cy="42%" r="62%"><stop offset="0" stop-color="#ffc35a"/><stop offset=".35" stop-color="#f06a1c"/><stop offset=".75" stop-color="#b8160b"/><stop offset="1" stop-color="#5e0604"/></radialGradient>
<radialGradient id="warm{i}" cx="50%" cy="40%" r="65%"><stop offset="0" stop-color="#f2521a"/><stop offset=".55" stop-color="#b3150c"/><stop offset="1" stop-color="#4a0403"/></radialGradient>
<radialGradient id="dim{i}" cx="50%" cy="40%" r="65%"><stop offset="0" stop-color="#c82a12"/><stop offset=".6" stop-color="#7e0b07"/><stop offset="1" stop-color="#2e0202"/></radialGradient>
<linearGradient id="gloss{i}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".55"/><stop offset=".45" stop-color="#fff" stop-opacity=".08"/><stop offset=".5" stop-color="#fff" stop-opacity="0"/></linearGradient>
<radialGradient id="tile{i}" cx="50%" cy="50%" r="70%"><stop offset="0" stop-color="#6a0a06"/><stop offset=".55" stop-color="#2a0302"/><stop offset="1" stop-color="#0c0101"/></radialGradient>
<radialGradient id="halo{i}" cx="50%" cy="50%" r="50%"><stop offset="0" stop-color="#ff5a1a" stop-opacity=".55"/><stop offset=".6" stop-color="#c0180a" stop-opacity=".18"/><stop offset="1" stop-color="#c0180a" stop-opacity="0"/></radialGradient>
<linearGradient id="flare{i}" x1="0" y1="1" x2="1" y2="0"><stop offset="0" stop-color="#ffb347" stop-opacity="0"/><stop offset=".5" stop-color="#fff6d8"/><stop offset="1" stop-color="#ffb347" stop-opacity="0"/></linearGradient>
<radialGradient id="star{i}"><stop offset="0" stop-color="#fff"/><stop offset=".3" stop-color="#ffe7a8" stop-opacity=".9"/><stop offset="1" stop-color="#ff9a30" stop-opacity="0"/></radialGradient>
<filter id="mot{i}" x="0" y="0" width="100%" height="100%"><feTurbulence type="fractalNoise" baseFrequency=".045" numOctaves="3" seed="7"/><feColorMatrix values="0 0 0 0 0  0 0 0 0 0  0 0 0 0 0  0 0 0 -1.6 1.15"/></filter>
<filter id="glow{i}" x="-40%" y="-40%" width="180%" height="180%"><feGaussianBlur stdDeviation="4"/></filter>
</defs>'''

def cell(i,cx,cy,r,fill,rim="#f6d2a8",rimw=None,gloss=True,detail=True):
    rimw = rimw if rimw is not None else max(r*0.09,1.0)
    p=hexpts(cx,cy,r); pi=hexpts(cx,cy,r-rimw*0.5)
    out=''
    if rim!="#2a0503" and r>12: out+=f'<polygon points="{p}" fill="none" stroke="#ff6a1e" stroke-width="{rimw*2.6:.2f}" filter="url(#glow{i})" opacity=".8"/>'
    out+=f'<polygon points="{p}" fill="url(#{fill}{i})" stroke="{rim}" stroke-width="{rimw:.2f}" stroke-linejoin="round"/>'
    if gloss:
        gp=hexpts(cx,cy,r*0.86)
        out+=f'<polygon points="{gp}" fill="url(#gloss{i})" />'
    return out

def mottle(i,cells,size,op=.55):
    m="".join(f'<polygon points="{hexpts(cx,cy,r*0.92)}" fill="#fff"/>' for cx,cy,r in cells)
    return f'<mask id="mm{i}">{m}</mask><rect width="{size}" height="{size}" filter="url(#mot{i})" mask="url(#mm{i})" opacity="{op}"/>'

def flare(i,cx,cy,L,w):
    a=math.radians(-55); dx,dy=math.cos(a)*L/2,math.sin(a)*L/2
    return (f'<line x1="{cx-dx:.1f}" y1="{cy-dy:.1f}" x2="{cx+dx:.1f}" y2="{cy+dy:.1f}" stroke="url(#flare{i})" stroke-width="{w}" stroke-linecap="round"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{w*4}" fill="url(#star{i})"/>')

# ---------- designs (all in a 256 box) ----------
def core(i,size,tile=False):
    o=[]; cells=[]
    if tile: o.append(f'<rect x="8" y="8" width="240" height="240" rx="44" fill="url(#tile{i})"/>')
    o.append(f'<circle cx="128" cy="128" r="118" fill="url(#halo{i})"/>')
    if size>=48:
        r=15; inner=[]
        for q in range(-4,5):
            for s in range(-6,7):
                x,y=grid(128,128,r,q,s)
                if abs(x-128)/70+abs(y-128)/62<=1.0: inner.append((x,y))
        outer=[(104,38),(160,30),(50,84),(212,96),(30,160),(78,190),(214,168),(176,206),(124,228)]
        ro=18
    elif size>=24:
        r=22; inner=[grid(128,128,r,q,s) for q,s in [(0,0),(0,-1),(0,1),(1,-1),(1,0),(-1,0),(-1,1)]]
        outer=[(78,58),(196,72),(52,176),(196,190),(124,230)]; ro=22
    else:
        r=40; inner=[(128,91),(128,165),(68,128),(188,128)]
        outer=[]; ro=32
    for x,y in outer:
        o.append(cell(i,x,y,ro,"dim",rimw=max(ro*.12,3 if size<48 else 2))); cells.append((x,y,ro))
    gl="".join(f'<polygon points="{hexpts(x,y,r*1.05)}"/>' for x,y in inner)
    o.append(f'<g fill="#ff7a2a" opacity=".7" filter="url(#glow{i})">{gl}</g>')
    for x,y in inner:
        d=math.hypot(x-128,y-128)/80
        f="hot" if d<.62 else "warm"
        o.append(cell(i,x,y,r*0.97,f,rim="#2a0503",rimw=max(r*.1,2.5 if size<48 else 1.6))); cells.append((x,y,r))
    if size>=48: o.append(mottle(i,cells,256,.45))
    if size>=24: o.append(flare(i,128,128,150 if size>=48 else 120, 4 if size>=48 else 7))
    return "".join(o)

def breaking(i,size,tile=False):
    o=[]; cells=[]
    if tile: o.append(f'<rect x="8" y="8" width="240" height="240" rx="44" fill="url(#tile{i})"/>')
    o.append(f'<circle cx="112" cy="140" r="110" fill="url(#halo{i})"/>')
    r=36 if size>=24 else 46
    keep=[(0,0),(0,-1),(-1,0),(-1,1),(0,1),(1,0)] if size>=24 else [(0,0),(-1,0),(0,1)]
    for q,s in keep:
        x,y=grid(108,142,r,q,s); f="hot" if (q,s)==(0,0) else "warm"
        o.append(cell(i,x,y,r*0.93,f,rimw=max(r*.08,4 if size<48 else 2.6))); cells.append((x,y,r*.93))
    # the breaking cell (would be q=1,s=-1) flies off up-right, rotated
    x,y=grid(108,142,r,1,-1)
    for (dx,dy,sc,rot) in ([(26,-22,.78,18),(66,-58,.38,-14)] if size>=24 else [(30,-26,.8,18)]):
        cx,cy=x+dx,y+dy
        o.append(f'<g transform="rotate({rot} {cx:.1f} {cy:.1f})">'+cell(i,cx,cy,r*sc,"dim",rimw=max(r*sc*.1,3.5 if size<48 else 2.4))+'</g>')
    if size>=48:
        o.append(mottle(i,cells,256,.4))
        # crack line where the cell broke away
        cx,cy=grid(108,142,r,0,0)
        o.append(f'<path d="M{cx+r*.47:.0f} {cy-r*.8:.0f} l-7 11 6 7 -9 13 4 6" fill="none" stroke="#2a0302" stroke-width="2.2" stroke-linejoin="round" opacity=".85"/>')
    return "".join(o)

def single(i,size,tile=False):
    o=[]
    if tile: o.append(f'<rect x="8" y="8" width="240" height="240" rx="44" fill="url(#tile{i})"/>')
    o.append(f'<circle cx="128" cy="128" r="122" fill="url(#halo{i})"/>')
    r=100 if size>=24 else 110
    o.append(cell(i,128,128,r,"hot",rimw=6 if size>=48 else 11))
    # inner comb etched in the cell
    if size>=32:
        ir=r*0.22; inner=[]
        for q in range(-3,4):
            for s in range(-3,4):
                x,y=grid(128,128,ir,q,s)
                if math.hypot(x-128,y-128)<r*0.62: inner.append((x,y))
        o.append('<g opacity=".55">'+"".join(f'<polygon points="{hexpts(x,y,ir*0.9)}" fill="none" stroke="#3a0402" stroke-width="{2.4 if size>=48 else 4}"/>' for x,y in inner)+'</g>')
    if size>=48: o.append(mottle(i,[(128,128,r)],256,.35))
    o.append(flare(i,128,128,200 if size>=48 else 170, 5 if size>=48 else 9))
    return "".join(o)

def crest(i,size,tile=False):
    o=[]; cells=[]
    if tile: o.append(f'<rect x="8" y="8" width="240" height="240" rx="44" fill="url(#tile{i})"/>')
    o.append(f'<circle cx="128" cy="128" r="120" fill="url(#halo{i})"/>')
    r=26 if size>=24 else 40
    rng=[(q,s) for q in range(-3,4) for s in range(-4,5)]
    lim=1.0
    for q,s in rng:
        x,y=grid(128,128,r,q,s)
        if abs(x-128)/(108 if size>=24 else 100)+abs(y-128)/(112 if size>=24 else 100)<=lim:
            d=math.hypot(x-128,y-128)/100
            f="hot" if d<.3 else ("warm" if d<.7 else "dim")
            o.append(cell(i,x,y,r*0.9,f,rimw=max(r*.09,4 if size<48 else 2.2))); cells.append((x,y,r*.9))
    if size>=48: o.append(mottle(i,cells,256,.4))
    if size>=32: o.append(flare(i,128,128,170,5 if size>=48 else 8))
    return "".join(o)

def svg(fn,size,tile=False):
    uid[0]+=1; i=uid[0]
    return f'<svg width="{size}" height="{size}" viewBox="0 0 256 256">{defs(i)}{fn(i,size,tile)}</svg>'

designs=[("A","Core cluster",core,False),("B","Core cluster, red tile",core,True),("C","Breaking comb",breaking,False),("D","Breaking comb, red tile",breaking,True),("E","Single cell",single,False),("F","Comb crest",crest,False)]
if __name__=="__main__":  # a contact sheet of every design at every size
    rows=[]
    for k,name,fn,t in designs:
        row="".join(f'<div class="c">{svg(fn,s,t)}<span>{s}</span></div>' for s in (256,64,48,32,24,16))
        rows.append(f'<div class="row"><b>{k} · {name}</b>{row}</div>')
    html=f'''<html><body style="margin:0;background:#000;color:#aaa;font:13px sans-serif;padding:12px">
<style>.row{{display:flex;align-items:end;gap:18px;margin-bottom:14px}}.row b{{width:110px}}.c{{display:flex;flex-direction:column;align-items:center;gap:4px}}</style>
{"".join(rows)}</body></html>'''
    open("sheet.html","w").write(html)
