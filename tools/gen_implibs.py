#!/usr/bin/env python3
"""Generate .def files (for llvm-dlltool) from Wine's .spec export lists.

usage: gen_implibs.py WINE_SRC OUTDIR ARCH(x86_64|i386) dll...
"""
import os, re, sys

SIZES = {'long': 4, 'ptr': 4, 'str': 4, 'wstr': 4, 'segptr': 4, 'segstr': 4,
         'int64': 8, 'double': 8, 'float': 4, 'int128': 16, 'word': 4, 's_word': 4}

def parse(spec, arch):
    out = []
    for line in open(spec, encoding='latin-1'):
        line = line.split('#')[0].strip()
        if not line:
            continue
        toks = line.split()
        if len(toks) < 3:
            continue
        kind = toks[1]
        i = 2
        flags = []
        while i < len(toks) and toks[i].startswith('-'):
            flags.append(toks[i]); i += 1
        if i >= len(toks):
            continue
        if any(f.startswith('-arch=') for f in flags):
            archs = [f[6:] for f in flags if f.startswith('-arch=')][0]
            names = archs.split(',')
            ok = False
            for a in names:
                neg = a.startswith('!')
                a2 = a.lstrip('!')
                m = (a2 == arch or (a2 == 'win64' and arch == 'x86_64') or (a2 == 'win32' and arch == 'i386'))
                if neg and m: ok = False; break
                if not neg and m: ok = True
                if neg and not m: ok = True
            if not ok:
                continue
        if any(f == '-private' for f in flags):
            continue
        rest = ' '.join(toks[i:])
        if kind in ('stdcall', 'cdecl', 'varargs', 'thiscall', 'stub', 'extern', 'pascal'):
            m = re.match(r'([A-Za-z_][\w@?$]*)(\((.*?)\))?', rest)
            if not m:
                continue
            name = m.group(1)
            args = (m.group(3) or '').split()
            if kind == 'stub':
                continue
            nbytes = sum(SIZES.get(a, 4) for a in args)
            out.append((name, kind, nbytes))
    return out

def main():
    wine, outdir, arch = sys.argv[1], sys.argv[2], sys.argv[3]
    os.makedirs(outdir, exist_ok=True)
    for dll in sys.argv[4:]:
        spec = os.path.join(wine, 'dlls', dll, dll + '.spec')
        ents = parse(spec, arch)
        with open(os.path.join(outdir, dll + '.def'), 'w') as f:
            f.write('LIBRARY %s.dll\nEXPORTS\n' % dll)
            seen = set()
            for name, kind, nbytes in ents:
                if name in seen:
                    continue
                seen.add(name)
                if arch == 'i386' and kind == 'stdcall':
                    f.write('  %s@%d\n' % (name, nbytes))
                elif kind == 'extern':
                    f.write('  %s DATA\n' % name)
                else:
                    f.write('  %s\n' % name)
        print(dll, len(seen))

main()
