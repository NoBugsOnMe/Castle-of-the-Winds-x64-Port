#!/bin/bash
# loadgame.sh GAME LOGLEVEL - start the game and load the save selected by default in the Open dialog
"$(dirname "$0")/run.sh" $1 ${2:-0} 10
export DISPLAY=:99
timeout 60 xctl click 637 671 sleep 5000 click 640 1004 sleep 3000 click 508 350 sleep 4000
