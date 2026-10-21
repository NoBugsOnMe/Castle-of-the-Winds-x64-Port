#!/usr/bin/env python3
"""Minimal WinHelp 3.0/3.1 (.HLP) reader used to convert the Castle of the
Winds help files into the compact format rendered by the port's built-in
help viewer."""
import struct

def u8(b, o): return b[o]
def u16(b, o): return struct.unpack_from('<H', b, o)[0]
def s16(b, o): return struct.unpack_from('<h', b, o)[0]
def u32(b, o): return struct.unpack_from('<I', b, o)[0]
def s32(b, o): return struct.unpack_from('<i', b, o)[0]

def cstr(b, o):
    e = b.index(0, o)
    return b[o:e].decode('cp1252'), e + 1

def lz77(data, maxlen=None):
    out = bytearray()
    i = 0
    n = len(data)
    while i < n:
        flags = data[i]; i += 1
        for bit in range(8):
            if i >= n: break
            if flags & (1 << bit):
                if i + 1 >= n: i = n; break
                w = data[i] | (data[i + 1] << 8); i += 2
                length = (w >> 12) + 3
                pos = len(out) - (w & 0x0FFF) - 1
                for k in range(length):
                    out.append(out[pos + k] if pos + k >= 0 else 0)
            else:
                out.append(data[i]); i += 1
            if maxlen and len(out) >= maxlen: return bytes(out[:maxlen])
    return bytes(out)

def runlen(data):
    out = bytearray(); i = 0
    while i < len(data):
        n = data[i]; i += 1
        if n & 0x80:
            out += data[i:i + (n & 0x7F)]; i += n & 0x7F
        else:
            out += bytes([data[i]]) * n; i += 1
    return bytes(out)

class Reader:
    """reads compressed values from a bytes object"""
    def __init__(self, b, o=0): self.b = b; self.o = o
    def byte(self):
        v = self.b[self.o]; self.o += 1; return v
    def sbyte(self):
        v = self.b[self.o]; self.o += 1; return v - 256 if v > 127 else v
    def word(self):
        v = u16(self.b, self.o); self.o += 2; return v
    def sword(self):
        v = s16(self.b, self.o); self.o += 2; return v
    def dword(self):
        v = u32(self.b, self.o); self.o += 4; return v
    def sdword(self):
        v = s32(self.b, self.o); self.o += 4; return v
    def cushort(self):
        v = self.byte()
        return v // 2 if not (v & 1) else (v // 2) + 128 * self.byte()
    def csshort(self):
        v = self.byte()
        if not (v & 1): return v // 2 - 64
        return v // 2 + 128 * self.byte() - 16384
    def culong(self):
        v = self.word()
        return v // 2 if not (v & 1) else (v // 2) + 32768 * self.word()
    def cslong(self):
        v = self.word()
        if not (v & 1): return v // 2 - 16384
        return v // 2 + 32768 * self.word() - 67108864
    def cstr(self):
        s, self.o = cstr(self.b, self.o); return s

class HelpFile:
    def __init__(self, path):
        self.b = open(path, 'rb').read()
        b = self.b
        assert u32(b, 0) == 0x00035F3F, 'not a help file'
        self.files = {}
        self.read_btree(u32(b, 4) + 9, self.dir_leaf)
        self.read_system()
        self.read_phrases()
        self.read_fonts()

    # ---- internal file system ----
    def ifile(self, name):
        if name not in self.files:
            return None
        o = self.files[name]
        used = u32(self.b, o + 4)
        return self.b[o + 9:o + 9 + used]

    def read_btree(self, o, leaf_fn, data=None):
        b = data if data is not None else self.b
        magic = u16(b, o)
        assert magic == 0x293B, hex(magic)
        pagesize = u16(b, o + 4)
        root = s16(b, o + 26)
        nlevels = s16(b, o + 32)
        pages = o + 38
        page = root
        for _ in range(nlevels - 1):
            page = s16(b, pages + page * pagesize + 4)   # PreviousPage = first child
        while page != -1:
            p = pages + page * pagesize
            n = s16(b, p + 2)
            nxt = s16(b, p + 6)
            q = p + 8
            for _ in range(n):
                q = leaf_fn(b, q)
            page = nxt

    def dir_leaf(self, b, q):
        name, q = cstr(b, q)
        self.files[name] = u32(b, q)
        return q + 4

    def read_system(self):
        s = self.ifile('|SYSTEM')
        self.minor = u16(s, 2)
        self.flags = u16(s, 10)
        self.title = ''
        if self.minor <= 16:
            self.title, _ = cstr(s, 12)
            self.blocksize = 2048
            self.compressed = False
        else:
            self.blocksize = 4096 if self.flags in (0, 4) else 2048
            self.compressed = self.flags in (4, 8)
            o = 12
            while o + 4 <= len(s):
                rt, sz = u16(s, o), u16(s, o + 2)
                if rt == 1: self.title, _ = cstr(s, o + 4)
                o += 4 + sz

    def read_phrases(self):
        self.phrases = None
        p = self.ifile('|Phrases')
        if p is None: return
        n = u16(p, 0)
        if self.minor <= 16:
            offs = [u16(p, 4 + 2 * i) for i in range(n + 1)]
            base = 4
            text = p
            self.phrases = [text[base + offs[i]:base + offs[i + 1]] for i in range(n)]
        else:
            dsize = u32(p, 4)
            offs = [u16(p, 8 + 2 * i) for i in range(n + 1)]
            text = lz77(p[8 + 2 * (n + 1):], dsize)
            d0 = offs[0]
            self.phrases = [text[offs[i] - d0:offs[i + 1] - d0] for i in range(n)]

    def expand(self, data):
        if self.phrases is None: return data
        out = bytearray(); i = 0
        while i < len(data):
            c = data[i]; i += 1
            if 0 < c < 16:
                num = (c * 256 - 256 + data[i]); i += 1
                out += self.phrases[num // 2]
                if num & 1: out.append(32)
            else:
                out.append(c)
        return bytes(out)

    def read_fonts(self):
        f = self.ifile('|FONT')
        self.faces = []
        self.fonts = []
        if f is None: return
        nfaces, ndesc, foff, doff = struct.unpack_from('<HHHH', f, 0)
        flen = (doff - foff) // nfaces if nfaces else 0
        for i in range(nfaces):
            raw = f[foff + i * flen: foff + (i + 1) * flen]
            self.faces.append(raw.split(b'\0')[0].decode('cp1252'))
        for i in range(ndesc):
            o = doff + i * 11
            attr, half, fam, face = f[o], f[o + 1], f[o + 2], u16(f, o + 3)
            fg = f[o + 5:o + 8]
            self.fonts.append(dict(bold=bool(attr & 1), italic=bool(attr & 2), underline=bool(attr & 4),
                                   strike=bool(attr & 8), dunder=bool(attr & 16), smallcaps=bool(attr & 32),
                                   halfpoints=half, family=fam, face=self.faces[face] if face < len(self.faces) else 'Arial',
                                   color=(fg[0], fg[1], fg[2])))

    # ---- topics ----
    def topic_raw(self):
        if not hasattr(self, '_traw'):
            self._traw = self.ifile('|TOPIC')
        return self._traw

    def read_link_bytes(self, pos, length):
        """read bytes of a topic link starting at raw pos, skipping block headers"""
        t = self.topic_raw(); bs = self.blocksize
        out = bytearray()
        while len(out) < length and pos < len(t):
            if pos % bs < 12:
                pos = (pos // bs) * bs + 12
            take = min(length - len(out), bs - pos % bs)
            out += t[pos:pos + take]
            pos += take
        return bytes(out)

    def links(self):
        """yield (rawpos, recordtype, linkdata1, linkdata2) for all topic links (HC30 layout)"""
        t = self.topic_raw()
        pos = 12
        while pos < len(t):
            hdr = self.read_link_bytes(pos, 21)
            if len(hdr) < 21: break
            bsz, dl2, prev, nxt, dl1, rt = struct.unpack('<iiiiiB', hdr)
            if bsz <= 0: break
            data = self.read_link_bytes(pos, bsz)
            ld1 = data[21:dl1]
            ld2 = data[dl1:bsz]
            if dl2 > len(ld2):
                ld2 = self.expand(ld2)
            yield pos, rt, ld1, ld2
            if nxt <= 0: break
            pos += nxt

    def parse_text_record(self, rt, ld1, ld2):
        """return list of paragraphs: dict(fmt=..., items=[...])"""
        r = Reader(ld1)
        r.cslong()                      # TopicSize
        if rt in (0x20, 0x23):
            r.cushort()                 # TopicLength
        if rt == 0x23:
            raise NotImplementedError('tables')
        texts = ld2.split(b'\0')
        ti = 0
        paras = []
        def parainfo():
            r.byte(); r.byte(); r.word()
            bits = r.word()
            p = dict(above=0, below=0, lines=0, left=0, right=0, first=0, align='left', tabs=[], border=None)
            if bits & 0x0001: r.cslong()
            if bits & 0x0002: p['above'] = r.csshort()
            if bits & 0x0004: p['below'] = r.csshort()
            if bits & 0x0008: p['lines'] = r.csshort()
            if bits & 0x0010: p['left'] = r.csshort()
            if bits & 0x0020: p['right'] = r.csshort()
            if bits & 0x0040: p['first'] = r.csshort()
            if bits & 0x0100:
                bb = r.byte(); r.sword(); p['border'] = bb
            if bits & 0x0200:
                n = r.csshort()
                for _ in range(n):
                    ts = r.cushort()
                    tt = 0
                    if ts & 0x4000: tt = r.cushort()
                    p['tabs'].append((ts & 0x3FFF, tt))
            if bits & 0x0400: p['align'] = 'right'
            if bits & 0x0800: p['align'] = 'center'
            return p
        fmt = parainfo()
        cur = dict(fmt=fmt, items=[])
        def emit_text():
            nonlocal ti
            if ti < len(texts):
                s = texts[ti]; ti += 1
                if s: cur['items'].append(('text', s.decode('cp1252')))
        emit_text()
        while r.o < len(ld1):
            c = r.byte()
            if c == 0xFF:
                break
            elif c == 0x20: r.dword()
            elif c == 0x21: r.word()
            elif c == 0x80: cur['items'].append(('font', r.word()))
            elif c == 0x81: cur['items'].append(('br',))
            elif c == 0x82:
                paras.append(cur); cur = dict(fmt=fmt, items=[])
            elif c == 0x83: cur['items'].append(('tab',))
            elif c in (0x86, 0x87, 0x88):
                typ = r.byte()
                size = r.cslong() if False else r.culong()
                if typ == 0x22: r.cushort()
                start = r.o
                if typ in (3, 0x22):
                    embedded = r.word()
                    if embedded == 0:
                        num = r.word()
                        cur['items'].append(('bmp', {0x86: 'c', 0x87: 'l', 0x88: 'r'}[c], num))
                    else:
                        cur['items'].append(('bmpdata', {0x86: 'c', 0x87: 'l', 0x88: 'r'}[c], ld1[r.o:start + size]))
                r.o = start + size
            elif c == 0x89: cur['items'].append(('endlink',))
            elif c == 0x8B: cur['items'].append(('text', ' '))
            elif c == 0x8C: pass
            elif c in (0xC8, 0xCC):
                ln = r.word(); r.o += ln - 3
            elif c in (0xE0, 0xE1, 0xE2, 0xE3, 0xE6, 0xE7):
                target = r.dword()
                kind = 'popup' if c in (0xE0, 0xE2, 0xE6) else 'jump'
                cur['items'].append(('link', kind, target, c in (0xE6, 0xE7)))
            elif c in (0xEA, 0xEB, 0xEE, 0xEF):
                ln = r.word(); r.o += ln
            else:
                raise ValueError('unknown formatting command %02X at %d' % (c, r.o - 1))
            emit_text()
        if cur['items']: paras.append(cur)
        return paras

    def topics(self):
        """returns list of topics: dict(num, pos, title, prev, next, paras)"""
        tops = []
        cur = None
        num = 16
        for pos, rt, ld1, ld2 in self.links():
            if rt == 2:
                bsz, prevt, nextt = struct.unpack_from('<iii', ld1, 0)
                title = ld2.split(b'\0')[0].decode('cp1252')
                cur = dict(num=num, pos=pos, title=title, prev=prevt, next=nextt, paras=[])
                tops.append(cur)
                num += 1
            elif rt in (1, 0x20):
                cur['paras'] += self.parse_text_record(rt, ld1, ld2)
        return tops

    # ---- maps, keywords, pictures ----
    def context_map(self):
        d = self.ifile('|CTXOMAP')
        if d is None: return []
        n = u16(d, 0)
        return [(u32(d, 2 + 8 * i), u32(d, 6 + 8 * i)) for i in range(n)]

    def keywords(self):
        bt = self.files.get('|KWBTREE')
        data = self.ifile('|KWDATA')
        out = []
        if bt is None or data is None: return out
        def leaf(b, q):
            kw, q = cstr(b, q)
            cnt = u16(b, q); off = u32(b, q + 2)
            out.append((kw, [u32(data, off + 4 * k) for k in range(cnt)]))
            return q + 6
        self.read_btree(bt + 9, leaf)
        return out

    def picture(self, name):
        """returns a packed DIB (BITMAPINFOHEADER + palette + bits) or None"""
        d = self.ifile(name)
        if d is None: return None
        magic, npics = u16(d, 0), u16(d, 2)
        off = u32(d, 4)
        r = Reader(d, off)
        ptype = r.byte(); pack = r.byte()
        if ptype not in (5, 6): return None
        xdpi = r.culong(); ydpi = r.culong()
        planes = r.cushort(); bpp = r.cushort()
        w = r.culong(); h = r.culong()
        used = r.culong(); important = r.culong()
        csize = r.culong(); hsize = r.culong()
        coff = r.dword(); hoff = r.dword()
        ncolors = used if used else (1 << bpp if bpp <= 8 else 0)
        pal = b''
        if ptype == 6:
            pal = d[r.o:r.o + 4 * ncolors]
        raw = d[off + coff: off + coff + csize]
        if pack == 1: raw = runlen(raw)
        elif pack == 2: raw = lz77(raw)
        elif pack == 3: raw = runlen(lz77(raw))
        if ptype == 5:
            # DDB: convert monochrome only (others unlikely in these files)
            return None
        stride = ((w * bpp + 31) // 32) * 4
        raw = raw[:stride * h].ljust(stride * h, b'\0')
        bih = struct.pack('<IiiHHIIiiII', 40, w, h, 1, bpp, 0, len(raw), 3780, 3780, ncolors, 0)
        return bih + pal + raw
