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

## Scripted input and screenshots (session 6)

`xdotool` key events reach the game (the Wine desktop window holds X focus), and the game's own screenshot function writes `scrNNNNN.bmp` (640 × 480, 8-bit) when **Alt+C** is pressed (GNW key code 0x12E, set at `0x43A4B0`). `tools/diagnostics/drive_original.py` runs a list of timed actions (`wait`, `key`, `down`, `up`, `shot`) and collects the screenshots; with `WINEDEBUG=+file,+timestamp` the Wine trace gives millisecond timestamps for sound loads and for each screenshot.

A test scheme `testopen.sch` (all cells blank, density 0) was added to the **working copy only**, and `options.ini` there selects it with `random_start=0`.

Key sequence used to reach a match with two keyboard players and no AI: Enter ×3 (skip logos), Enter (Start Game), Down, Right, Right (player 2: AI → KEY 0 → KEY 1), Enter, Enter.

| # | Observation | Evidence | Confirms |
|---|---|---|---|
| D13 | Main menu items, top to bottom: Start Game, Start Network Game, Join Network Game, Options, About Bomberman, Online Manual, Exit Bomberman. "V1.0" is shown top-left | Screenshot | Resolves UNKNOWN-009 |
| D14 | Start Game leads to the player list first (Player 1: KEY 0, Player 2: AI, 3-10: OFF by default; list colours white, black, red, blue, green, yellow, cyan, magenta, orange, purple), then to a level/scheme screen ("Green Acres", "2 Wins to win match") with a scheme preview, then to the match | Screenshots | Screen order; `.rmp` colour order |
| D15 | On the player list, Right cycles a slot AI → KEY 0 → KEY 1 → OFF; Left sets OFF | Screenshots | `Match_PlayerSetupScreen` key handling |
| D16 | Default keys: set 0 = arrow keys, Space (bomb), Enter (action); set 1 = R/G/F/D for up/right/down/left, S (bomb). The codes in the exe are DirectInput scan codes (0xC8, 0xCD, 0xD0, 0xCB, 0x39, 0x1C; 0x13, 0x22, 0x21, 0x20, 0x1F), and arrows and Space do drive player 1 | `0x406158`-`0x4061BC`; movement and bomb drop observed | Gameplay keyboard goes through DirectInput (UNKNOWN-004, mostly) |
| D17 | **Walking speed ≈ 183 px/s.** Right held 2.0 s: 366 px. Left held 1.0 s: 184 px. Fitting both gives 182 px/s plus about 11 ms of key-timing overhead | Sprite offset between screenshots in the same pose (`tools/diagnostics/sprite_shift.py`) | Predicted 184.6 px/s at 50 ms ticks, 180 px/s at 1 ms ticks: the measurement lies between, as expected for the 1-5 ms ticks of this machine |
| D18 | **Single bomb: explosion at +2.000 s** after the drop sound | Timestamps of `bmdrop3.rss` and `explo1.rss` | T2 |
| D19 | **Flames last about 500 ms.** Screenshots at +2.334 s and +2.473 s show the flames; at +2.612 s they are gone | Timestamped screenshots | T3 (lifetime between 473 and 612 ms; predicted 500) |
| D20 | **Blast shape**: bomb in cell (3,0) with the starting range: flames on the bomb cell, two cells west, two cells east, two cells south, none north (field edge). The player standing in cell (0,0), next to the westmost flame, survives | Screenshots | E1, E4; death is by cell, not by sprite overlap |
| D21 | A dropped bomb is drawn in its owner's colour (white for player 1) | Screenshots | Colour remap applies to bombs |
| D22 | The round clock shows 2:25 five seconds into the match and counts down in seconds | Screenshots | Value 100 = 150 s |

## Lab scheme experiments (session 7)

`testlab.sch` (working copy only): the open field with born-with 3 bombs and 1 kicker. For the closing-walls run `playtime=70` was set temporarily.

| # | Observation | Evidence | Confirms |
|---|---|---|---|
| D23 | Scheme "born with" values take effect: the player could place a second bomb and kick | Second `bmdrop` sound accepted; kick sound | Scheme → values 50+type |
| D24 | **Direction priority.** From the corner (0,0), Right+Down held: the player moves down only. Then Right+Up held in open field: the player moves right only | Screenshots | M7: south beats east, east beats north |
| D25 | **Chain reaction.** Bombs dropped 0.418 s apart in adjacent cells: explosion sounds at +2.000 s and +2.005 s | Sound-load timestamps | B4/B5: the second bomb is detonated by the first, about one tick later (ticks are 1-5 ms on this machine), not at its own fuse time |
| D26 | **Kick.** Walking into the bomb with a kicker: kick sound at +0.763 s, bomb-stop sound at +1.159 s; the bomb travelled from cell 2 to cell 0 (80 px) in 0.396 s = 202 px/s and came to rest at the centre of the edge cell. Its explosion still came at +2.000 s after the drop | Timestamps, screenshots | B9: speed 1000 = 200 px/s, stops at the last free cell centre, fuse runs while sliding |
| D27 | **Hurry and closing walls.** With a 70 s clock: hurry voice at +10.01 s (clock passes below 60 s); first wall block at +14.26 s, i.e. 0.25 s after the clock reaches 55 s; 71 blocks in the next 17.5 s with a mean spacing of 0.2500 s (min 0.201, max 0.299) | Timestamps of `zai01c.rss` and `sqrdrop*.rss` | R1; the 250 ms cadence |
| D28 | **Spiral shape.** After 16 blocks the whole top row is solid and the right column has begun; after 36 the top row, right column and most of the bottom row; after 69 the outer ring is complete and the second ring's top row and right column are filling. Players standing inside are unharmed | Screenshots | R1, R2 (clockwise from the top-left corner, ring by ring) |

| D29 | **Time up is a draw.** Two idle players, enclosement depth 0, 1:00 on the clock: the clock ran down with nothing else happening (no walls at 55 s), and 59.02 s after the round started the game showed "DRAW GAME" (`draw.pcx`, `draw.rss`) | Screenshots, timestamps | Resolves UNKNOWN-022; depth 0 disables the walls |
| D30 | `playtime=12` in `options.ini` produced a clock starting at 1:00, so short values are raised to 60 s (or rejected in favour of 60) | Screenshot showing 0:51 at +8.5 s | – |

## Punch, grab and spooge (session 8)

Lab schemes `testpun.sch`, `testgrab.sch`, `testspg.sch` (working copy only) give the player the powerup from the start.

| # | Observation | Evidence | Confirms |
|---|---|---|---|
| D31 | **Punch.** Bomb in cell 5, player in cell 6 facing west, Enter pressed: punch sound (`kbomb*.rss`), the bomb rises in an arc and lands in cell 2, three cells away. Flight time 0.458 s and 0.461 s in two trials. The first bomb exploded 1.131 s after landing, having used 0.868 s of fuse before the punch: 1.999 s in total | Timestamps of `kbomb`, the landing sound (`bmdrop3.rss`) and the explosion; rapid screenshots | B13: three-cell first hop at 260 px/s (predicted 0.4615 s); the fuse is paused in flight |
| D32 | **Grab and throw.** Space on the player's own bomb with Space held: grab sound (`grab2.rss`), the bomb is carried above the head and follows the player; still unexploded 3.2 s after being dropped. On release it flies three cells in the facing direction (landing sound 0.46 s later) and explodes 1.999 s after landing | Screenshots, timestamps | B16: held bombs do not tick; the fuse restarts when thrown |
| D33 | **Spooge.** Space on the player's own bomb with capacity 5: four more bombs appear in a line in the facing direction, one per cell | Screenshot | B18 (placement and capacity limit; the 50 ms stagger could not be read from sound loads) |
| D34 | Sound loads are cached: a sound file is opened only the first time it plays, so file-open timestamps mark the *first* occurrence of each sound only | `bmdrop3.rss` opened once for many bounces | Method note |

| D35 | **Corner slide.** Pillars-only scheme (`testpil.sch`, working copy). From (0,0), Down for 0.15 s leaves the player about 27 px below the row-0 line, beside the pillar at (1,1). Holding Right then moves the player back up into row 0 and on to the east (ends near x = 135 in row 0) instead of being stuck against the pillar | Screenshots | M5 |
| D36 | **Lane alignment.** From there (x about 15 px past the centre of column 2), holding Down moves the player down between the pillars and pulls it onto the centre of column 2 | Screenshots | M4 |

| D37 | **Conveyor.** Level 10 (`levelno=10`), open scheme. The belt cells of `extra10.res` are drawn as soon as the cells are open. A player standing on the east-moving belt in row 2 without input is carried east: about 115 px in 1.62 s and 114 px in 1.63 s, i.e. 70 px/s, keeping its facing | Screenshots with file timestamps | Extras: idle player carried at the belt speed (value 191 = 350 → 70 px/s) |

| D38 | **Team play.** `team_play=1`, two players (teams 0 and 1 from the scheme): player 1 is drawn white, player 2 red instead of black, and each score label is in the team colour | Screenshots from about 3 s into the round | Team colours: team 0 → colour 0, team 1 → colour 2. The opening "true colours" period (value 32) was not caught |
| D39 | **Field edge.** At (0,0), holding Up and then Left for 0.4 s each leaves the player in the same place (only the facing changes) | Screenshot difference confined to the sprite | M3 for the edge case |
| D40 | **Capacity.** With one bomb on the field and capacity 1, pressing Space again two cells away places nothing | Screenshot shows a single bomb | B1 |
| D41 | **A bomb blocks the way back.** After walking two cells away from the bomb and holding Left for 0.6 s (enough for 110 px), the player stands at the centre of the cell next to the bomb. The player then dies in the blast | Screenshots | M8; flames kill in adjacent cells within range |

| D42 | **Trigger bombs.** Born with trigger and capacity 2 (`testtrg.sch`): the two bombs dropped are drawn as trigger bombs and are still there 3.3 s later. Enter detonates the first; the second, two cells away and inside its range, goes with it. A third bomb dropped afterwards is an ordinary bomb and explodes by itself 2 s later | Screenshots | B17: no fuse, button 2 detonates the oldest, only "capacity" trigger bombs per pickup |
| D43 | **Jelly bomb.** Born with jelly and kicker (`testjel.sch`): the kicked bomb slides west to the field edge and comes back east | Six rapid screenshots: bomb x = 75, 52, 45, 65, 82, 118 | B11 |
| D44 | **Goldflame.** Born with goldflame (`testgold.sch`): a bomb at (0,0) fills the whole of row 0 and the whole of column 0 with flame; a player at (1,1) is unharmed | Screenshots | E9 |

| D45 | **Warp.** Level 4, open scheme: four holes at (2,2), (12,2), (2,8), (12,8). Walking east into the hole at (2,2) (id 0, link-to 3): 0.1 s later the player is on that hole, 0.3 s after that the player stands on the hole at (2,8) (id 3) | Screenshots | Extras: warp linkage by id, travel time under half a second |
| D46 | **Trampoline.** Level 9, open scheme: eight trampolines are drawn (four at the fixed cells, four elsewhere). After walking onto the one at (2,2) the player is not on screen 0.6 s later and stands at about (1,3) 2 s later | Screenshots | Extras: airborne phase, landing within two cells in a different row and column |

Unexplained in the D46 run: the idle second player appears at different cells in the three screenshots. It may have started on, or been carried onto, one of the randomly placed trampolines; not investigated.

| D47 | **Scheme override and reveal.** `testpow.sch` (working copy): five bricks in row 0, powerup overrides set so that only skates are generated, five of them. Burning the first brick reveals a skate under it | Screenshots | Scheme override of the per-level counts; powerups hide under bricks; a blast stops at the brick |
| D48 | **Pickup and skate speed.** Walking into the revealed skate's cell removes it. Afterwards, holding Right for 1.5 s moves the player 320 px: about 212 px/s (without a skate the same hold gives about 276 px) | Screenshots, sprite offset | Pickup by cell; M2: 214.6 px/s nominal with one skate, 210 at 1 ms ticks |

| D49 | **Arrows.** Level 3, open scheme, player with a kicker. A bomb kicked east along row 0 from cell 1 is seen 0.77 s later in column 4 just below row 0, 1.14 s later in row 2 near column 5, and 1.45 s later in column 6 above row 2. That is the route east → south at the arrow on (4,0) → east at (4,2) → north at (6,2), and the distances (154, 228 and 290 px along the route) match 200 px/s. The bomb exploded at +2.003 s after the drop | Timestamped screenshots, `extra3.res` | Extras: arrows turn a sliding bomb at the cell centre; kick speed; fuse runs while sliding |

| D50 | **Flying bomb meets a player.** `testhit.sch` (working copy) starts player 2 on (2,0) and player 1 on (5,0) with punch. Player 1 drops a bomb on (5,0), steps to (6,0), turns back and punches: punch sound at +1.314 s, a bounce sound 0.459 s later (three cells, where player 2 stands) and another 0.153 s after that (one more cell); the bomb is then at rest on (1,0), beyond player 2. It explodes at +2.611 s: 1.314 s of fuse before the punch plus 0.685 s after landing = 1.999 s | Timestamps, screenshots | B15: a flying bomb does not land on a player, it makes a further one-cell hop (40 px at 260 px/s = 0.154 s); fuse paused in flight |

Unexplained, from a first attempt at D50 in which one key press was lost and the bomb was never punched: the explosion sound loaded 1.864 s after the drop sound instead of 2.000 s. Every other measurement of an unpunched bomb gave 2.000 s. Not reproduced; cause unknown.

| D51 | **Both players killed by one bomb.** Player 1's bomb on (4,0) kills player 1 on (4,0) and player 2 on (2,0) in the same instant (+2.000 s, death sounds for both). The round clock stops, the death animations play, and 3.7 s later the "DRAW GAME" screen appears. Player 1's kill count stays 0 (one kill, one suicide) | Timestamps, screenshots | Round result: nobody left is a draw; kill scoring +1 / −1 |

## Not yet possible

Attaching a debugger, and reading game memory directly. Remaining scenarios of `docs/testing/original-behaviour.md` can now be scripted one by one.
