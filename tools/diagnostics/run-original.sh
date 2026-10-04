#!/bin/sh
# Runs the original bm95.exe under Wine from a writable working copy, inside a
# 640x480 virtual desktop, and closes it gracefully after N seconds so that the
# game's own debug.log is flushed.
#
#   tools/diagnostics/run-original.sh [seconds] [WINEDEBUG channels]
#
# The working copy lives in work/run (ignored by git); game/ is never touched.
# With no input the game shows three logo screens, the main menu, and after
# 30 s of idling starts an AI demo match ("attract mode").
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SECS=${1:-120}
CHANNELS=${2:--all}
RUN="$ROOT/work/run"
export WINEPREFIX="$ROOT/work/wineprefix"
export PATH="/usr/lib/i386-linux-gnu/wine:$PATH"   # wineserver lives here on Ubuntu

if [ ! -d "$RUN" ]; then
    mkdir -p "$ROOT/work"
    cp -a "$ROOT/game" "$RUN"
    chmod -R u+w "$RUN"
    # Enable the game's debug log; disable sound output and networking.
    printf 'hdhome=.\r\ncdhome=.\r\ndebug=3debug.log\r\nsoundonoff=0\r\nnetonoff=0\r\n\032' > "$RUN/cfg.ini"
    # Without options.ini the game stops at a memory-configuration prompt.
    printf ';Bomberman Options file.\r\nlevelno=0\r\nnum_to_win_match=2\r\nenclosement_depth=1\r\nconveyor_speed=1\r\nteam_play=0\r\nrandom_start=1\r\nstomped_bombs_detonate=1\r\nwin_by_kills=0\r\ngoldman=0\r\nschemefilename=BASIC.SCH\r\nplaytime=150\r\nassign_keyboards=1\r\ndiseases_destroyable=1\r\nlost_net_revert_ai=0\r\ndisable_game_music=1\r\nsmallmemory=0\r\n' > "$RUN/options.ini"
fi
if [ ! -d "$WINEPREFIX" ]; then
    WINEDEBUG=-all wineboot -i
    WINEDEBUG=-all wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d Default /f >/dev/null
    WINEDEBUG=-all wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v Default /d 640x480 /f >/dev/null
fi

cd "$RUN"
rm -f debug.log
(WINEDEBUG="$CHANNELS" wine bm95.exe > "$ROOT/work/trace.log" 2>&1 &)
timeout "$SECS" wineserver -w || true
WINEDEBUG=-all wine taskkill /IM bm95.exe >/dev/null 2>&1 || true
timeout 15 wineserver -w || wineserver -k
echo "debug log: $RUN/debug.log   wine trace: $ROOT/work/trace.log"
