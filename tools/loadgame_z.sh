#!/bin/bash
# loadgame_z.sh GAME LOGLEVEL [FILE] - start the game and load a saved game (any desktop size / zoom)
D=$(dirname "$0")
"$D/run.sh" $1 ${2:-0} 10
export DISPLAY=:99
wclick Button '^OK$' > /dev/null; sleep 4
wclick Button 'Load Saved Game' > /dev/null; sleep 4
python3 - "${3:-rick.cwg}" <<'PY'
import re, subprocess, sys
out = subprocess.run(['wininfo'], capture_output=True, text=True, errors='replace').stdout
for line in out.splitlines():
    m = re.match(r"\s*([0-9A-F]+) \[Edit\] '.*?' style=(\w+) .*?\((-?\d+),(-?\d+)\)-\((-?\d+),(-?\d+)\) id=1148", line)
    if m:
        x = (int(m.group(3)) + int(m.group(5))) // 2; y = (int(m.group(4)) + int(m.group(6))) // 2
        subprocess.run(['/usr/local/bin/xctl', 'click', str(x), str(y), 'sleep', '300', 'type', sys.argv[1], 'sleep', '300', 'key', 'Return'], env={'DISPLAY': ':99'})
        break
PY
sleep 5
