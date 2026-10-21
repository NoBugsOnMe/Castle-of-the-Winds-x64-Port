import sys, subprocess, re, os
sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from nedump import NE, pstr, u16
from specmap import specs
path, outdir = sys.argv[1], sys.argv[2]
os.makedirs(outdir, exist_ok=True)
n=NE(path)
def target_desc(r):
    t=r['flags']&3
    if t==1:
        mod=n.mods[r['a']-1]; nm=specs.get(mod,{}).get(r['c'],(0,'ord%d'%r['c']))[1]; return '%s.%s'%(mod,nm)
    if t==2:
        nm,_=pstr(n.b,n.ne+n.h['impname_off']+r['c']); return '%s.%s'%(n.mods[r['a']-1],nm)
    if t==0:
        if r['a']&0xff==0xff:
            e=n.entries.get(r['c']); return 'entry%d(seg%d:%04x)'%(r['c'],e['seg'],e['off'])
        return 'seg%d:%04x'%(r['a']&0xff, r['c'])
    return 'osfixup%d'%r['a']
allfix={}
for s in n.segs:
    data=bytearray(n.b[s['fileoff']:s['fileoff']+s['len']])
    fix={}
    for r in n.relocs(s):
        desc=target_desc(r)
        if r['flags']&4:  # additive
            fix[r['off']]=(r['stype'],desc,True)
        else:
            o=r['off']; seen=0
            while o!=0xffff and o < len(data) and seen<10000:
                nxt=u16(data,o); fix[o]=(r['stype'],desc,False); o=nxt; seen+=1
    allfix[s['idx']]=fix
    fn=os.path.join(outdir,'seg%02d.bin'%s['idx'])
    open(fn,'wb').write(data)
    if s['flags']&1: continue
    lst=subprocess.run(['ndisasm','-b16',fn],capture_output=True,text=True).stdout
    out=[]
    for line in lst.splitlines():
        m=re.match(r'^([0-9A-F]{8})\s+([0-9A-F]+)\s+(.*)$',line)
        if not m: out.append(line); continue
        addr=int(m.group(1),16); nbytes=len(m.group(2))//2
        ann=[]
        for k in range(addr,addr+nbytes):
            if k in fix: ann.append('[%s %s]'%({3:'FAR',2:'SEL',5:'OFF',0:'LOB'}.get(fix[k][0],fix[k][0]),fix[k][1]))
        out.append('%04X  %-18s %s %s'%(addr,m.group(2),m.group(3),' '.join(ann)))
    open(os.path.join(outdir,'seg%02d.asm'%s['idx']),'w').write('\n'.join(out)+'\n')
print('entries:')
for o,e in sorted(n.entries.items()):
    nm=[x for x,y in n.resnames+n.nonres if y==o]
    print(o,e,nm)
