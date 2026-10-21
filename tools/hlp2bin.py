#!/usr/bin/env python3
"""Convert a WinHelp 3.x file into the compact blob used by src/help.c.

usage: hlp2bin.py INPUT.HLP OUTPUT.bin [--html OUTPUT.html]
"""
import sys, struct, html
sys.path.insert(0, __import__('os').path.dirname(__file__))
from hlp import HelpFile

def s(b):
    raw = b.encode('cp1252', 'replace')
    return struct.pack('<H', len(raw)) + raw

def main():
    h = HelpFile(sys.argv[1])
    tops = h.topics()
    num2idx = {t['num']: i for i, t in enumerate(tops)}
    pos2idx = {t['pos']: i for i, t in enumerate(tops)}
    bmp_names = sorted({it[2] for t in tops for p in t['paras'] for it in p['items'] if it[0] == 'bmp'})
    bmp_index = {}
    bitmaps = []
    for n in bmp_names:
        dib = h.picture('bm%d' % n) or h.picture('|bm%d' % n)
        if dib:
            bmp_index[n] = len(bitmaps)
            bitmaps.append(dib)
    out = bytearray(b'CHLP')
    cmap = [(m, pos2idx[t]) for m, t in h.context_map() if t in pos2idx]
    kws = [(k, [pos2idx[t] for t in ts if t in pos2idx]) for k, ts in h.keywords()]
    out += struct.pack('<HHHHHHH', 1, len(tops), len(h.fonts), len(bitmaps), len(cmap), len(kws), 0)
    out += s(h.title)
    for f in h.fonts:
        flags = (1 if f['bold'] else 0) | (2 if f['italic'] else 0) | (4 if f['underline'] or f['dunder'] else 0) | (8 if f['strike'] else 0)
        out += struct.pack('<BBBB', flags, f['halfpoints'], f['family'], 0) + s(f['face'])
    for t in tops:
        def idx(n): return num2idx.get(n, -1)
        out += struct.pack('<Hhh', t['num'], idx(t['prev']), idx(t['next'])) + s(t['title'])
        out += struct.pack('<H', len(t['paras']))
        for p in t['paras']:
            f = p['fmt']
            al = {'left': 0, 'center': 1, 'right': 2}[f['align']]
            out += struct.pack('<BBhhhhh', al, len(f['tabs']), f['left'], f['right'], f['first'], f['above'], f['below'])
            for pos, typ in f['tabs']:
                out += struct.pack('<hB', pos, typ)
            items = bytearray(); n = 0
            for it in p['items']:
                k = it[0]
                if k == 'text': items += b'\0' + s(it[1])
                elif k == 'font': items += b'\1' + struct.pack('<H', it[1])
                elif k == 'br': items += b'\2'
                elif k == 'tab': items += b'\3'
                elif k == 'link':
                    items += b'\4' + struct.pack('<BhB', 1 if it[1] == 'popup' else 0, num2idx.get(it[2], -1), 1 if it[3] else 0)
                elif k == 'endlink': items += b'\5'
                elif k == 'bmp':
                    if it[2] not in bmp_index: continue
                    items += b'\6' + struct.pack('<BH', 'clr'.index(it[1]), bmp_index[it[2]])
                else: continue
                n += 1
            out += struct.pack('<H', n) + items
    for b in bitmaps:
        out += struct.pack('<I', len(b)) + b
    for m, t in cmap:
        out += struct.pack('<IH', m, t)
    for k, ts in kws:
        out += s(k) + struct.pack('<H', len(ts)) + b''.join(struct.pack('<H', t) for t in ts)
    open(sys.argv[2], 'wb').write(out)
    print('%s: %d topics, %d fonts, %d bitmaps, %d map entries, %d keywords, %d bytes' %
          (sys.argv[1], len(tops), len(h.fonts), len(bitmaps), len(cmap), len(kws), len(out)))

main()
