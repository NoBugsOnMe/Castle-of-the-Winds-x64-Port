#!/usr/bin/env python3
"""List / extract RT_GROUP_ICON resources from an NE file as .ico."""
import sys, struct
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from nedump import NE

def find(n, typ, rid):
    for r in n.resources:
        if r['type'] == typ and r['id'] == rid:
            return n.b[r['off']:r['off'] + r['len']]
    return None

def extract(n, name, out):
    grp = find(n, 14, name)
    cnt = struct.unpack_from('<H', grp, 4)[0]
    entries = []
    for i in range(cnt):
        e = grp[6 + 14 * i: 6 + 14 * (i + 1)]
        w, h, cc, res, planes, bpp, size, nid = struct.unpack('<BBBBHHIH', e)
        data = find(n, 3, nid)
        entries.append((w, h, cc, planes, bpp, data))
    hdr = struct.pack('<HHH', 0, 1, len(entries))
    off = 6 + 16 * len(entries)
    dirs = b''; blobs = b''
    for (w, h, cc, planes, bpp, data) in entries:
        dirs += struct.pack('<BBBBHHII', w, h, cc, 0, planes, bpp, len(data), off + len(blobs))
        blobs += data
    open(out, 'wb').write(hdr + dirs + blobs)

if __name__ == '__main__':
    n = NE(sys.argv[1])
    if len(sys.argv) == 2:
        names = [r['id'] for r in n.resources if r['type'] == 14]
        print([x for x in names if isinstance(x, str)], len(names))
    else:
        name = sys.argv[2]
        try: name = int(name)
        except ValueError: pass
        extract(n, name, sys.argv[3])
