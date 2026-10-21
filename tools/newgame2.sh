#!/bin/bash
# newgame2.sh LOGLEVEL - CASTLE2: create a character and get past the learn-spell dialogs and the story text
D=$(dirname "$0")
"$D/newgame_z.sh" 2 ${1:-0}
export DISPLAY=:99
for i in $(seq 1 10); do wclick Button '^OK$' > /dev/null || break; sleep 2; done
