#!/usr/bin/env python3
"""thunk.py <addr...>: kernel import name for a thunk slot address (call dword ptr [slot])."""
import json, re, sys, os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
x = json.load(open(os.path.join(root, 'analysis', 'xbe.json')))
src = open(os.path.join(root, 'sdk', 'runtime', 'kernel', 'ordinals.cpp')).read()
names = [None] + re.findall(r'"(\w+)"', src[src.index('kNames'):])
for a in sys.argv[1:]:
    i = (int(a, 16) - x['kernel_thunk']) // 4
    o = x['kernel_imports'][i]
    print(a, o, names[o])
