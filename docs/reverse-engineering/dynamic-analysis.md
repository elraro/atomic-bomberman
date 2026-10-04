# Dynamic Analysis

Observations of the original running. These are the first Level 2 findings.

## Setup

- Wine 10.0 (Ubuntu 26.04, `wine` + `wine32:i386`), private prefix `work/wineprefix`, 640 × 480 virtual desktop.
- The game runs from a writable copy `work/run`; `game/` is untouched. Procedure: `tools/diagnostics/run-original.sh`.
- `cfg.ini` in the copy: `debug=3debug.log` (level 3 = log to the named file), `soundonoff=0`, `netonoff=0`.
- An `options.ini` must exist. Without it the game stops in a "high/low memory" selection screen (`0x406086`) and waits for a key.
- No input is injected (no tool for it is installed). Left alone, the game shows the logo screens, the main menu, and after 30 s starts an AI demo match. All gameplay observations below come from demo matches.
- No screenshots: screen capture of the XWayland root fails. Evidence is taken from Wine traces (`+file`, `+ddraw`, with `+timestamp`) and from the game's `debug.log`. The log is only flushed on a clean exit; `taskkill /IM bm95.exe` (WM_CLOSE) achieves that.

## Observations

| # | Observation | Evidence | Confirms |
|---|---|---|---|
| D1 | The game starts and runs under Wine without patches | Loads `bm95.exe`, `DINPUT`, `DDRAW`, `DSOUND`; runs for minutes | – |
| D2 | DirectDraw: `SetCooperativeLevel` flags 0x11 (fullscreen, exclusive), `SetDisplayMode(640, 480, 8)`, 8-bit palettised primary surface, palette created and set; frames are written through `Lock`/`Unlock` only | `+ddraw` trace | Graphics API and mode: now CONFIRMED |
| D3 | `sizeof(OBJ) = 152`; 338 values and 316 messages loaded; 2027 sounds found, 1051 listed | `debug.log` | Static readings |
| D4 | Resource names ending `.plt` are opened as `.pcx` (`winz.plt` → `.\data\res\winz.pcx`); sounds are requested as `name.snd` and opened as `.rss` | `debug.log` "NM:" lines | Path builder (resolves UNKNOWN-008 in outline) |
| D5 | Startup file order: `cfg.ini`, `valuelst.res`, `messages.txt`, `font0…9.fon` (only 0, 1, 6 exist), `color.pal`, `winz.pcx`, `0…9.rmp`, `options.ini`, `master.ali` and the `.ani` files, `soundlst.res` | `+file` trace | Init order in `Game_InitAll` |
| D6 | Logo screens `iplogo`, `hslogo`, `title` are shown 7-8 s each | File-open timestamps 7, 8 and 8 s apart | Value 12 = 7 s |
| D7 | Attract mode starts 30.0 s after the main menu appears | `mainmenu.pcx` → `basic.sch`/`field3.pcx` | Value 92 = 30 s |
| D8 | **Bomb fuse = 2.000 s.** In a 64 s demo match with 115 bomb drops and 85 explosions, 84 drops have an explosion within 120 ms of exactly +2.000 s (median offset 0 ms); pairing all drops with all explosions gives a single sharp peak in the 2.00 s bin (84, against a background of about 8 per bin) | Timestamps of `bmdrop*.rss` and `bomb_*.rss` opens; `tools/diagnostics/fuse_from_trace.py` | Value 41 × 50 ms; fuse is time-based, not tick-based |
| D9 | The first bomb is dropped 1.15 s after the level background is loaded | Same trace | Consistent with the 1000 ms start freeze |
| D10 | The game presents about **960 times per second** on this machine, in the menu and during a match (median gap between surface locks 1.0 ms) | 47 735 `Lock` calls in the traced run | No frame limiter (UNKNOWN-013): CONFIRMED |
| D11 | `joyGetNumDevs()` returns 16 under Wine; the game uses 10 | `debug.log` | – |
| D12 | The demo match used level 3 with `extra3.res` (arrows) and scheme `basic.sch` | `debug.log`, file trace | Extras loading |

## What D10 means

On a modern machine a tick lasts about 1 ms. Time-based rules are unaffected (D8: the fuse is still exactly 2 s). Tick-counted rules collapse: a chain reaction advances one bomb per millisecond, and the 16-tick stun lasts 16 ms. Movement uses `speed × dt / 50` with truncation, so at dt = 1 ms the default speed 923 yields 18 units per tick instead of 18.46: about 2.5 % slower than at 50 ms ticks.

The original was therefore tuned for machines that produced roughly its nominal 20 frames per second (value 25: "nominal frame rate used as a reference"). For the modern implementation a fixed 50 ms simulation step reproduces the designed behaviour of the tick-counted rules; rendering can interpolate. This is a design recommendation, not yet a measurement on period hardware.

## Not yet possible

Driving the game with scripted input (needs an X input tool such as `xdotool`, or a Wine-side helper), screenshots, and attaching a debugger. With input, the scenarios in `docs/testing/original-behaviour.md` can be run one by one.
