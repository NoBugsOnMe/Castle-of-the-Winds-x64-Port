#!/bin/bash
# newgame_z.sh GAME LOGLEVEL - start the game and create a character (any desktop size / zoom),
# clicking controls by name through wininfo
D=$(dirname "$0")
"$D/run.sh" $1 ${2:-0} 10
export DISPLAY=:99
wclick Button '^OK$' > /dev/null; sleep 4
wclick Button 'Start New Game' > /dev/null; sleep 4
# focus the name field and type
python3 - <<'PY'
import re, subprocess
out = subprocess.run(['wininfo'], capture_output=True, text=True, errors='replace').stdout
for line in out.splitlines():
    m = re.match(r"\s*([0-9A-F]+) \[Edit\] '.*?' style=(\w+) .*?\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\) id=151", line)
    if m:
        x = (int(m.group(3)) + int(m.group(5))) // 2; y = (int(m.group(4)) + int(m.group(6))) // 2
        subprocess.run(['/usr/local/bin/xctl', 'click', str(x), str(y), 'sleep', '300', 'type', 'Rick'], env={'DISPLAY': ':99'})
        break
PY
sleep 0.5
wclick Button '^OK$' > /dev/null; sleep 3
wclick Button '&Yes' > /dev/null; sleep 4
wclick Button '^OK$' > /dev/null; sleep 4
