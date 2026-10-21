#!/bin/bash
# shot.sh NAME [crop WxH+X+Y] - screenshot the virtual display
DISPLAY=:99 import -window root /tmp/claude-0/$1.png
if [ -n "$2" ]; then convert /tmp/claude-0/$1.png -crop $2 +repage /tmp/claude-0/$1.png; fi
echo /tmp/claude-0/$1.png
