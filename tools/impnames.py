import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from nedump import NE
from specmap import specs
from collections import defaultdict
res={}
for path in sys.argv[1:]:
    n=NE(path)
    for s in n.segs:
        for r in n.relocs(s):
            if r['flags']&3==1:
                mod=n.mods[r['a']-1]; o=r['c']
                res[(mod,o)]=specs[mod].get(o,('?','?','',''))
for (mod,o),(k,name,args,impl) in sorted(res.items()):
    print('%-8s %4d %-8s %-28s (%s) -> %s'%(mod,o,k,name,args,impl))
print(len(res))
