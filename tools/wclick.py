#!/usr/bin/env python3
# wclick.py CLASS TEXT_REGEX [r|d] [N] - click the centre of the Nth visible window matching class and text
# (install as /usr/local/bin/wclick; needs xctl and wininfo in /usr/local/bin)
import re, subprocess, sys
cls, pat = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else ''
nth = int(sys.argv[4]) if len(sys.argv) > 4 else 0
out = subprocess.run(['wininfo'], capture_output=True, text=True, errors='replace').stdout
hits = []
for line in out.splitlines():
    m = re.match(r"\s*([0-9A-F]+) \[(.*?)\] '(.*?)' style=(\w+) ex=(\w+) vis=(\d) en=(\d) \((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\)", line)
    if not m: continue
    if m.group(2) != cls or m.group(6) != '1' or not re.search(pat, m.group(3)): continue
    x1, y1, x2, y2 = map(int, m.group(8, 9, 10, 11))
    hits.append(((x1 + x2) // 2, (y1 + y2) // 2, line.strip()))
if len(hits) <= nth:
    print('no match for', cls, pat); sys.exit(1)
x, y, line = hits[nth]
print('click', x, y, line[:100])
cmd = {'r': 'rclick', 'd': 'dclick'}.get(mode, 'click')
subprocess.run(['/usr/local/bin/xctl', cmd, str(x), str(y)], env={'DISPLAY': ':99', 'PATH': '/usr/local/bin:/usr/bin:/bin'})
