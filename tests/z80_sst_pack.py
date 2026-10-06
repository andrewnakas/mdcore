#!/usr/bin/env python3
"""Packs SingleStepTests/z80 v1 JSON into one flat binary for tests/z80_sst.c.
Test-only: the vectors are not part of mdcore and are not redistributed."""
import json, struct, sys, os
src, out = sys.argv[1], sys.argv[2]
K = ['pc','sp','a','b','c','d','e','f','h','l','i','r','ix','iy','af_','bc_','de_','hl_','im','iff1','iff2']
with open(out, 'wb') as o:
    for fn in sorted(os.listdir(src)):
        if not fn.endswith('.json'): continue
        for t in json.load(open(os.path.join(src, fn))):
            name = fn[:-5].encode()[:15].ljust(16, b'\0')
            b = [name]
            for st in (t['initial'], t['final']):
                b.append(struct.pack('<21H', *[st[k] for k in K]))
            ports = t.get('ports', [])
            b.append(struct.pack('<4H', len(t['cycles']), len(t['initial']['ram']), len(t['final']['ram']), len(ports)))
            for st in (t['initial'], t['final']):
                for a, v in st['ram']: b.append(struct.pack('<HH', a, v))
            for a, v, d in ports: b.append(struct.pack('<HBB', a, v, 1 if d == 'w' else 0))
            o.write(b''.join(b))
