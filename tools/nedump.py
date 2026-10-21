#!/usr/bin/env python3
"""Minimal NE (16-bit Windows) executable dumper used to plan the port."""
import struct, sys, json

def u8(b,o): return b[o]
def u16(b,o): return struct.unpack_from('<H',b,o)[0]
def u32(b,o): return struct.unpack_from('<I',b,o)[0]

def pstr(b,o):
    n=b[o]; return b[o+1:o+1+n].decode('latin-1'), o+1+n

class NE:
    def __init__(self, path):
        self.path=path
        b=self.b=open(path,'rb').read()
        assert b[:2]==b'MZ'
        ne=self.ne=u32(b,0x3c)
        assert b[ne:ne+2]==b'NE', 'not NE'
        h={}
        h['linkver']=(b[ne+2],b[ne+3])
        h['entry_off']=u16(b,ne+4); h['entry_len']=u16(b,ne+6)
        h['flags']=u16(b,ne+0xc); h['autodata']=u16(b,ne+0xe)
        h['heap']=u16(b,ne+0x10); h['stack']=u16(b,ne+0x12)
        h['ip']=u16(b,ne+0x14); h['cs']=u16(b,ne+0x16)
        h['sp']=u16(b,ne+0x18); h['ss']=u16(b,ne+0x1a)
        h['nseg']=u16(b,ne+0x1c); h['nmod']=u16(b,ne+0x1e)
        h['nonres_size']=u16(b,ne+0x20)
        h['seg_off']=u16(b,ne+0x22); h['res_off']=u16(b,ne+0x24)
        h['resname_off']=u16(b,ne+0x26); h['modref_off']=u16(b,ne+0x28)
        h['impname_off']=u16(b,ne+0x2a); h['nonres_off']=u32(b,ne+0x2c)
        h['movable_entries']=u16(b,ne+0x30); h['align']=u16(b,ne+0x32)
        h['nres_seg']=u16(b,ne+0x34); h['os']=b[ne+0x36]; h['flags2']=b[ne+0x37]
        h['winver']=u16(b,ne+0x3e)
        self.h=h
        # modules
        self.mods=[]
        for i in range(h['nmod']):
            off=u16(b,ne+h['modref_off']+2*i)
            s,_=pstr(b,ne+h['impname_off']+off); self.mods.append(s)
        # segments
        self.segs=[]
        for i in range(h['nseg']):
            o=ne+h['seg_off']+8*i
            sec,ln,fl,mn=struct.unpack_from('<HHHH',b,o)
            fileoff=sec<<h['align']
            ln = ln if (ln or not sec) else 0x10000
            if sec==0: ln=0
            mn = mn if mn else 0x10000
            self.segs.append(dict(idx=i+1,fileoff=fileoff,len=ln,flags=fl,minalloc=mn))
        # resident / nonresident names
        self.resnames=[]; o=ne+h['resname_off']
        while b[o]:
            s,o=pstr(b,o); ordn=u16(b,o); o+=2; self.resnames.append((s,ordn))
        self.nonres=[]; o=h['nonres_off']; end=o+h['nonres_size']
        while o<end and b[o]:
            s,o=pstr(b,o); ordn=u16(b,o); o+=2; self.nonres.append((s,ordn))
        # entry table
        self.entries={}; o=ne+h['entry_off']; ordn=1; end=o+h['entry_len']
        while o<end:
            cnt=b[o]; 
            if cnt==0: break
            typ=b[o+1]; o+=2
            if typ==0: ordn+=cnt; continue
            for k in range(cnt):
                if typ==0xff:
                    fl=b[o]; seg=b[o+3]; off=u16(b,o+4); o+=6
                else:
                    fl=b[o]; seg=typ; off=u16(b,o+1); o+=3
                self.entries[ordn]=dict(seg=seg,off=off,flags=fl,movable=(typ==0xff)); ordn+=1
        # resources
        self.resources=[]
        o=ne+h['res_off']
        if h['res_off']!=h['resname_off']:
            rshift=u16(b,o); o+=2
            while True:
                tid=u16(b,o)
                if tid==0: break
                cnt=u16(b,o+2); o+=8
                tname = tid&0x7fff if tid&0x8000 else pstr(b,ne+h['res_off']+tid)[0]
                for k in range(cnt):
                    roff,rlen,rfl,rid=struct.unpack_from('<HHHH',b,o); o+=12
                    rname = rid&0x7fff if rid&0x8000 else pstr(b,ne+h['res_off']+rid)[0]
                    self.resources.append(dict(type=tname,id=rname,off=roff<<rshift,len=rlen<<rshift,flags=rfl))
    def relocs(self, seg):
        b=self.b
        if not (seg['flags']&0x100) or seg['fileoff']==0: return []
        o=seg['fileoff']+seg['len']
        n=u16(b,o); o+=2; out=[]
        for i in range(n):
            st,fl,soff,a,c=struct.unpack_from('<BBHHH',b,o); o+=8
            out.append(dict(stype=st,flags=fl,off=soff,a=a,c=c))
        return out

if __name__=='__main__':
    n=NE(sys.argv[1])
    print(json.dumps(n.h,indent=0))
    print('modules',n.mods)
    print('resnames',n.resnames[:5],'... total',len(n.resnames))
    print('nonres',n.nonres[:5],'... total',len(n.nonres))
    print('entries',len(n.entries))
    tot=0
    for s in n.segs:
        r=n.relocs(s); tot+=len(r)
        print('seg %2d off=%06x len=%05x min=%05x flags=%04x %s relocs=%d'%(s['idx'],s['fileoff'],s['len'],s['minalloc'],s['flags'],'DATA' if s['flags']&1 else 'CODE',len(r)))
    print('total relocs',tot)
    from collections import Counter
    print('resource types',Counter(str(r['type']) for r in n.resources))
