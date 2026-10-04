# Original Behaviour Test Matrix

Reproducible scenarios for the core mechanics. They serve two purposes: experiments to run on the original once it can be executed, and the basis for the modern implementation's tests.

**Status of every "Expected" below: predicted from static analysis, not yet observed on the original** (see `../reverse-engineering/unknowns.md`, UNKNOWN-001). When a scenario is run on the original, record the result in the "Observed" column and promote or correct the corresponding rule in `../specifications/`.

Conventions: cells are (column, row) from the top-left, 0-based; the grid is 15 × 11; a frame is 50 ms; defaults from `valuelst.res` (speed 923, fuse 40 frames, range 2, 1 bomb). "Empty arena" means a scheme with no bricks and the standard solid pillars at odd (column, row).

## Timing

| Id | Scenario | Expected | Confidence | Observed |
|---|---|---|---|---|
| T1 | Start a round, hold a direction from the first moment | No movement for the first 1000 ms | HIGH | |
| T2 | Drop a bomb, measure to detonation | 2000 ms | HIGH | |
| T3 | Measure how long a flame is visible and lethal | 500 ms | HIGH | |
| T4 | Destroy a brick; measure until the cell is walkable | 500 ms after the blast | HIGH | |
| T5 | Cause a stall longer than 150 ms (e.g. disk access) mid-round | Game objects advance by at most 150 ms; the round clock loses the full stall | HIGH | |

## Movement

| Id | Scenario | Expected | Confidence | Observed |
|---|---|---|---|---|
| M1 | Empty arena, player at (0,0), hold east for 1 s after the start freeze | Moves about 185 px (4.6 cells) | HIGH | |
| M2 | Same with 1, 2, 3, 4 skates | 214, 244, 274, 304 px per second (speed 1073…1523) | HIGH | |
| M3 | Hold east into a solid pillar from a cell centre | Stops exactly at the cell centre; cannot advance past it | HIGH | |
| M4 | Player in a horizontal corridor, a few pixels above the row's centre line, hold east | Moves diagonally down-right until aligned, then straight | HIGH | |
| M5 | Player at a cell centre next to a pillar to the east, offset a few pixels north of the centre line, the cell to the north and the cell north-east both open; hold east | Slides north along the pillar, then continues east in the next lane | HIGH | |
| M6 | Hold east and south together where only south is open | Moves south | HIGH | |
| M7 | Hold two directions that are both open, in each of the 6 pairs | West beats south beats east beats north | HIGH | |
| M8 | Stand on own bomb, then walk off; try to walk back onto it | Leaving is allowed; re-entering is blocked at the neighbouring cell's centre | HIGH | |
| M9 | Hockey rink level: tap a direction | Movement starts 250 ms after the key press | MEDIUM | |

## Bombs

| Id | Scenario | Expected | Confidence | Observed |
|---|---|---|---|---|
| B1 | Capacity 1: drop a bomb, press again in another cell | Second press does nothing until the first bomb has exploded | HIGH | |
| B2 | Press bomb while standing on a bomb | Nothing (cell not passable) unless grab or spooge applies | HIGH | |
| B3 | Drop a bomb while walking, at various offsets within a cell | Bomb appears at the centre of the cell containing the player's reference point | HIGH | |
| B4 | Two bombs in adjacent cells, second dropped 1 s after the first | Both gone by the first bomb's detonation time plus one tick | HIGH | |
| B5 | Line of N bombs in adjacent cells, detonate the first | They explode one per tick in order along the line | HIGH | |
| B6 | Bomb A (player 1) chain-detonates bomb B (player 2); B's flame kills player 3 | Kill credited to player 1 | HIGH | |
| B7 | Bomb between two others, chain arrives from the west | The middle bomb sends no flame westward | HIGH | |
| B8 | Kill all but one player while bombs are ticking | Remaining bombs stop counting down | HIGH | |
| B9 | Kick a bomb down an open corridor | Slides at 10 px per frame (200 px/s) until the next cell is blocked, then rests at a cell centre | HIGH | |
| B10 | Kick a bomb toward a cell that currently has a flame | Bomb stops and explodes on the next tick | HIGH | |
| B11 | Kick a jelly bomb at a wall | Reverses and keeps sliding | HIGH | |
| B12 | Kicked bomb, press button 2 | Stops at the next cell centre (not jelly) | HIGH | |
| B13 | Punch a bomb with open field ahead | Lands 3 cells away if that cell is free, otherwise keeps hopping one cell at a time | MEDIUM | |
| B14 | Punch a bomb off the edge of the field | Reappears on the opposite side | MEDIUM | |
| B15 | Punch a bomb so that it lands on a player | Player stunned and loses 1-3 powerups, which reappear on the field; bomb bounces on | HIGH | |
| B16 | Hold a grabbed bomb for longer than the fuse | Does not explode while held; fuse restarts from zero when thrown | HIGH | |
| B17 | With a trigger powerup and capacity 3, lay 5 bombs over time | The first 3 are trigger bombs, later ones are normal | HIGH | |
| B18 | Spooge with capacity 4 in an open corridor | A line of bombs ahead of the player; they detonate 50 ms apart moving away from the player | HIGH | |

## Explosions

| Id | Scenario | Expected | Confidence | Observed |
|---|---|---|---|---|
| E1 | Range 2 bomb in open field | Flames on the bomb's cell and 2 cells in each direction | HIGH | |
| E2 | Bomb adjacent to a solid tile | No flame on the solid tile, none beyond | HIGH | |
| E3 | Bomb with a brick 1 cell away and another brick behind it | Only the first brick burns | HIGH | |
| E4 | Bomb at the edge of the field | Blast stops at the edge | HIGH | |
| E5 | Revealed powerup 1 cell from a range-3 bomb | Powerup destroyed; no flame on its cell or beyond | HIGH | |
| E6 | Player standing with the reference point 1 px inside a flamed cell | Dies | HIGH | |
| E7 | Player sprite overlapping a flamed cell but reference point in the neighbouring cell | Survives | HIGH | |
| E8 | Player walks into a cell 400 ms after its flame appeared | Dies (flame still active until 500 ms) | HIGH | |
| E9 | Goldflame bomb in an open row | Flames across the whole row and column up to the first obstacle | HIGH | |
| E10 | Player on a trampoline or mid-warp in a flamed cell | Survives | HIGH | |

## Powerups

| Id | Scenario | Expected | Confidence | Observed |
|---|---|---|---|---|
| P1 | Scheme with density 100 vs 50 | All brick cells filled vs about half | HIGH | |
| P2 | Count powerups revealed over many rounds with default settings | Exactly 10 bombs, 10 flames, 3 diseases, 4 kickers, 8 skates, 2 punches, 2 grabs, 1 spooge, 1 jelly when enough bricks exist; goldflame, trigger, super disease and random appear only sometimes | HIGH | |
| P3 | Burn a brick hiding a punch or grab in the first 40 s | A different powerup (or none) appears | HIGH | |
| P4 | Collect a 9th bomb powerup | Capacity stays 8 | HIGH | |
| P5 | Collect punch while holding trigger (and the other exclusive pairs) | The earlier one is lost and reappears on the field | HIGH | |
| P6 | Get a disease, then stand next to a healthy player | Healthy player becomes infected when within 30 px horizontally and 26 px vertically | HIGH | |
| P7 | Time a disease | Ends after 15 s | MEDIUM-HIGH | |
| P8 | Pick up 10 powerups while diseased | The disease is cured early in roughly 1 pickup out of 10 | HIGH | |

## Round

| Id | Scenario | Expected | Confidence | Observed |
|---|---|---|---|---|
| R1 | Let the clock run down with default settings | "Hurry" at 60 s left; walls start closing at 55 s left from the top-left corner, clockwise, 4 cells per second | HIGH | |
| R2 | Enclosement depth 1 | Two rings close, then the walls stop | MEDIUM-HIGH | |
| R3 | Stand in a cell as the wall reaches it | Dies | HIGH | |
| R4 | Bomb in a cell as the wall reaches it, default options | Bomb detonates | HIGH | |
| R5 | Start cells in a scheme full of bricks | The start cell and its non-solid orthogonal neighbours are clear when the round begins | HIGH | |
