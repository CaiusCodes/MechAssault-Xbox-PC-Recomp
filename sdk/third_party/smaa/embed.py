"""Writes sdk/runtime/gpu/smaa_hlsl.h: SMAA.hlsl as a C++ string, in pieces short enough for any compiler.
python sdk/third_party/smaa/embed.py"""
import os

here = os.path.dirname(os.path.abspath(__file__))
src = open(os.path.join(here, 'SMAA.hlsl'), encoding='latin-1').read().replace('\r\n', '\n')
src = ''.join(c if ord(c) < 128 else ' ' for c in src)  # a few Latin-1 bytes in its comments
out = os.path.join(here, '..', '..', 'runtime', 'gpu', 'smaa_hlsl.h')
pieces = []
while src:
    cut = src[:12000]
    if len(src) > 12000:
        cut = cut[:cut.rfind('\n') + 1]
    pieces.append(cut)
    src = src[len(cut):]
with open(out, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// mechassault-recomp patch: SMAA.hlsl (sdk/third_party/smaa, MIT), made by sdk/third_party/smaa/embed.py\n')
    f.write('#pragma once\n\ninline const char kSmaaSource[] =\n')
    for p in pieces:
        f.write('    R"SMAA(' + p + ')SMAA"\n')
    f.write('    ;\n')
print(len(pieces), 'pieces')
