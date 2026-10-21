#!/bin/bash
# tomine.sh - from the saved game in the hamlet, walk to mine level 0 (CASTLE1)
export DISPLAY=:99
for i in $(seq 1 17); do xctl key KP_8 sleep 250; done; sleep 1
for i in $(seq 1 15); do xctl key KP_9 sleep 250; done; sleep 1
for i in $(seq 1 3); do xctl key KP_8 sleep 250; done; sleep 1
xctl key KP_7 sleep 400 key shift+KP_8 sleep 6000
