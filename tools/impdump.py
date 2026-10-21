import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from nedump import NE, pstr, u16
from collections import defaultdict
n=NE(sys.argv[1])
imps=defaultdict(set); kinds=defaultdict(int); internal=0; osfix=0
for s in n.segs:
    for r in n.relocs(s):
        t=r['flags']&3; kinds[(r['stype'],r['flags'])]+=1
        if t==1: imps[n.mods[r['a']-1]].add(r['c'])
        elif t==2:
            nm,_=pstr(n.b,n.ne+n.h['impname_off']+r['c']); imps[n.mods[r['a']-1]].add(nm)
        elif t==0: internal+=1
        else: osfix+=1
print('reloc kinds (srctype,flags):',dict(kinds))
print('internal',internal,'osfixups',osfix)
for m,v in imps.items():
    print(m, len(v), sorted(v, key=lambda x: (isinstance(x,str), x)))
