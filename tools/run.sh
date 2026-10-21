#!/bin/bash
# run.sh GAME(1|2) [LOGLEVEL] [WAIT] - start the port under Wine on the virtual display
# (test harness; WINE_BUILD, WINEPREFIX and TESTDIR can be overridden)
WINE_BUILD=${WINE_BUILD:-/home/claude/wine-build}
TESTDIR=${TESTDIR:-/root/test}
export DISPLAY=${DISPLAY:-:99} WINEPREFIX=${WINEPREFIX:-/root/wpfx} WINEDEBUG=-all
"$WINE_BUILD/server/wineserver" -k 2>/dev/null
sleep 1
cd "$TESTDIR"
rm -f cotw-port.log
export COTW_LOG=${2:-0}
WINPATH=$("$WINE_BUILD/wine" winepath -w "$TESTDIR/CASTLE$1.EXE" 2>/dev/null)
nohup "$WINE_BUILD/wine" explorer /desktop=cotw,${DESK:-1280x1024} "$WINPATH" > "$TESTDIR/wine.out" 2>&1 &
sleep ${3:-6}
