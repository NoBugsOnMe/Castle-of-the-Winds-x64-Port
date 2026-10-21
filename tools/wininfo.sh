#!/bin/bash
# wininfo.sh [args] - dump the window tree on the cotw desktop (install as /usr/local/bin/wininfo)
# args: -h HWND X Y (hit test), -r HWND (redraw), -p X Y (window at point)
WINE_BUILD=${WINE_BUILD:-/home/claude/wine-build}
TESTDIR=${TESTDIR:-/root/test}
export DISPLAY=${DISPLAY:-:99} WINEPREFIX=${WINEPREFIX:-/root/wpfx} WINEDEBUG=-all
"$WINE_BUILD/wine" "$TESTDIR/wininfo.exe" "$@" 2>/dev/null
