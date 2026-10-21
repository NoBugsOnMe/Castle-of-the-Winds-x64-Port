import sys; sys.path.insert(0, __import__('os').path.dirname(__import__('os').path.abspath(__file__)))
from nedump import NE
n=NE(sys.argv[1])
for r in n.resources:
    if str(r['type']) in ('260','261','10','4','5','9','6','2') :
        d=n.b[r['off']:r['off']+r['len']]
        if str(r['type'])=='2' and not r['id'] in ('X',): 
            # bitmap header
            import struct
            hs=struct.unpack_from('<I',d,0)[0]
            if hs==40:
                w,h,pl,bpp=struct.unpack_from('<iiHH',d,4); print('BMP',r['id'],w,h,bpp,r['len'],hex(r['flags']))
            else: print('BMP?',r['id'],hs,r['len'])
            continue
        print(r['type'],r['id'],r['len'],hex(r['flags']),d[:48].hex(' '))
