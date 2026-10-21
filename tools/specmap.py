import re, sys
def load(path):
    m={}
    for line in open(path, encoding='latin-1'):
        line=line.split('#')[0].strip()
        if not line: continue
        mm=re.match(r'^(\d+|@)\s+(\S+)\s+(?:-\S+\s+)*(\S+?)(\((.*?)\))?(\s+(\S+))?\s*$', line)
        if not mm: continue
        ordn, kind, name = mm.group(1), mm.group(2), mm.group(3)
        if ordn=='@': continue
        m[int(ordn)]=(kind, name, (mm.group(5) or '').strip(), (mm.group(7) or ''))
    return m
import os
W=os.path.join(os.environ.get('WINE_SRC', '/home/claude/wine-mirror/wine'), 'dlls') + '/'
specs={'KERNEL':load(W+'krnl386.exe16/krnl386.exe16.spec'),'USER':load(W+'user.exe16/user.exe16.spec'),
       'GDI':load(W+'gdi.exe16/gdi.exe16.spec'),'COMMDLG':load(W+'commdlg.dll16/commdlg.dll16.spec')}
