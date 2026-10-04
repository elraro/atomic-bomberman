#!/usr/bin/env python3
"""Drive the original bm95.exe under Wine with scripted keys and capture its own screenshots.

Usage: drive_original.py OUT_DIR ACTION [ACTION...]
Actions:
  wait:SECONDS         sleep
  key:KEYS             xdotool key sequence, e.g. key:Return  key:alt+c
  down:KEY / up:KEY    hold / release a key
  shot:NAME            press Alt+C (the game's screenshot key) and save the frame as OUT_DIR/NAME.png
Requires the working copy and Wine prefix created by run-original.sh, xdotool and Pillow.
The script only sends keys while the Wine desktop / game window has X keyboard focus,
and aborts otherwise, so that synthetic keys never reach other programs.
"""
import glob
import os
import subprocess
import sys
import time

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RUN = os.path.join(ROOT, "work", "run")
ENV = dict(os.environ, WINEPREFIX=os.path.join(ROOT, "work", "wineprefix"),
           PATH="/usr/lib/i386-linux-gnu/wine:" + os.environ["PATH"])


class FocusLost(Exception):
    pass


def game_has_focus():
    """True only if the X keyboard focus is on the Wine desktop / game window."""
    r = subprocess.run(["xdotool", "getwindowfocus", "getwindowname"], env=ENV, capture_output=True, text=True)
    name = r.stdout.strip()
    return r.returncode == 0 and ("Wine Desktop" in name or "Atomic Bomberman" in name)


def xdo(*args):
    # Synthetic keys go to whatever window has the focus. Never send them unless
    # that window is the game: otherwise they would land in the user's own programs.
    if not game_has_focus():
        raise FocusLost()
    subprocess.run(["xdotool", *args], env=ENV, check=False, stderr=subprocess.DEVNULL)


def main():
    out, actions = sys.argv[1], sys.argv[2:]
    os.makedirs(out, exist_ok=True)
    for f in glob.glob(os.path.join(RUN, "scr*.bmp")) + [os.path.join(RUN, "debug.log")]:
        if os.path.exists(f):
            os.remove(f)
    channels = os.environ.get("WINEDEBUG", "-all")
    trace = open(os.path.join(ROOT, "work", "trace.log"), "w")
    game = subprocess.Popen(["wine", "bm95.exe"], cwd=RUN, env=dict(ENV, WINEDEBUG=channels),
                            stdout=trace, stderr=subprocess.STDOUT)
    try:
        time.sleep(3)
        if not game_has_focus():
            print("ABORT: the game window does not have keyboard focus; no keys were sent.")
            actions = []
        for a in actions:
            kind, _, arg = a.partition(":")
            if kind == "wait":
                time.sleep(float(arg))
            elif kind == "key":
                xdo("key", arg)
            elif kind == "down":
                xdo("keydown", arg)
            elif kind == "up":
                xdo("keyup", arg)
            elif kind == "shot":
                xdo("key", "alt+c")
                # The game writes scrNNNNN.bmp using the first unused number.
                found = None
                for _ in range(60):
                    done = [f for f in glob.glob(os.path.join(RUN, "scr*.bmp")) if os.path.getsize(f) > 300000]
                    if done:
                        found = done[0]
                        break
                    time.sleep(0.05)
                if found:
                    Image.open(found).convert("RGB").save(os.path.join(out, arg + ".png"))
                    os.remove(found)
                else:
                    print("no screenshot for", arg)
            else:
                print("unknown action", a)
    except FocusLost:
        print("ABORT: keyboard focus left the game window; stopped sending keys.")
    finally:
        quiet = dict(ENV, WINEDEBUG="-all")
        subprocess.run(["wine", "taskkill", "/IM", "bm95.exe"], env=quiet, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        try:
            game.wait(timeout=15)
        except subprocess.TimeoutExpired:
            subprocess.run(["wineserver", "-k"], env=quiet)
        trace.close()


if __name__ == "__main__":
    main()
