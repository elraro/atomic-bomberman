# Level Extras

Static analysis (Level 1) plus the data files. Sources: loader `0x404E99`, `Extra_FindAtCell` (`0x405654`), warp destination `0x405A81`, `Extras_Update` (`0x4056CA`), and the uses inside `Player_MoveSteps`, `Player_Update` and `Bombs_Update`.

## Data

`data/res/extraN.res` for level N; a level without the file has no extras.

| Level | File contents |
|---|---|
| 2 (hockey rink) | 12 arrows |
| 3 (ancient Egypt) | 44 arrows |
| 4 (coal mine) | 4 warp holes, linked in a ring 0 → 3 → 2 → 1 → 0 |
| 9 (deep forest) | 8 trampolines: 4 at fixed cells, 4 at random cells (`-T,H,H`) |
| 10 (inner city) | 32 conveyor cells |

Record formats and coordinate wrapping are in `src/resources/scheme_file.hpp`. Trampolines are skipped in network games.

## Storage

Up to 100 `OBJ` records (pointer `0x45E0A8`): `+0x04` type (0 arrow, 1 warp, 2 conveyor, 3 trampoline), `+0x1C`/`+0x20` the **cell** (not pixels), `+0x2C` direction (arrow, conveyor) or id (warp), `+0x2E` link-to id (warp). Confidence: HIGH.

## Behaviour

| Extra | Rule | Confidence |
|---|---|---|
| All | Drawn, and reachable, only while their cell's tile is blank. Bricks can cover them | HIGH |
| Arrow | A sliding bomb that reaches the centre of the arrow's cell takes the arrow's direction | HIGH; observed on the original through three turns (D49) |
| Conveyor | A resting bomb on a conveyor cell moves in the belt's direction at the belt speed, with the same stopping rules as a kicked bomb. A player walking with the belt gains the belt speed, against it loses it; a player giving no direction is carried along. Belt speed: values 190-192 (250 / 350 / 450) selected by the `conveyor_speed` option | HIGH; the idle-player case was observed on the original at 70 px/s (D37) |
| Warp | On its first update a warp turns its own cell and one random in-grid neighbour into blank tiles. A player who reaches one pixel before the centre of the warp's cell enters it: 9 frames of "spin" animation, then the player is placed at the centre of the warp whose id equals this warp's link-to, then 9 more frames. No input and no death meanwhile | HIGH for timing and linkage, observed on the original (D45); MEDIUM for the exact trigger point (an argument was lost in decompilation) |
| Trampoline | Same trigger point. The player flies for value 680 (30) frames, drawn value 681 (35) px higher per frame up to the midpoint and lower after it. At the midpoint the player is moved to a random cell within ±2 columns and ±2 rows that differs in both, is blank and holds no bomb (up to 100 tries). No input and no death meanwhile | HIGH; flight and landing observed (D46) |

Sliding bombs treat a warp cell as blocked; flying bombs do not land on one (`bombs.md`).

## Modern implementation

`World::setExtras`, with `Extra` records read by `parseExtrasText`. The application loads `extraN.res` for the chosen `--level`. Tests cover each kind. Not implemented: bombs falling into warps (not observed in the original's code either), the trampoline's spring animation length (a fixed 12 frames is used), and the warp "spin" animation.
