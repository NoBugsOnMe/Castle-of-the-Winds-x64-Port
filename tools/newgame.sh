#!/bin/bash
# newgame.sh GAME LOGLEVEL - start the game and create a character
"$(dirname "$0")/run.sh" $1 ${2:-0} 10
export DISPLAY=:99
timeout 60 xctl click 637 671 sleep 4500 click 465 1003 sleep 3000 click 350 80 type Rick sleep 300 click 130 434 sleep 2500 click 607 535 sleep 3000 click 220 177 sleep 300 click 244 336 sleep 3000
