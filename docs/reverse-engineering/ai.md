# Computer Players (AI)

Static analysis (Level 1). Sources: `ai.c` / `search.c` region `0x4090E7`-`0x40BEE6`.

## Structure

Confidence: HIGH.

- `Player_Update` calls the AI entry `0x40A1C6` instead of reading input when the player's controller type is 1.
- Per-player AI state is a 0x44-byte record (array at `0x45ED6C`, current record pointer at `0x45ED70`). The first word is the "personality"; only personality 0 exists (value 900 = 1), anything else is a fatal error.
- A personality is a **null-terminated table of behaviour functions** (`0x45BA78`). Each tick they are called in order; the first that returns non-zero ends the tick. A behaviour acts by writing the player's requested direction (`OBJ+0x2E`) and button flags (`OBJ+0x38`, `+0x39`, clearing the "previous" flags so the press counts as new).
- Ghidra had not found these eight functions (they are only reached through the table). They were created with a script in session 9.
- Before the behaviours run, `0x4090E7` rebuilds a passability grid (`0x45E724`: blocked if the tile is not blank or a bomb is in the cell).

## Danger map

One integer per cell (`0x4621F4`), cleared at the start of every bomb update, then raised (never lowered) by:

- every bomb, for each cell its blast would reach, at level `100 + elapsed fuse in ms` (`0x42429B`-`0x4242DE`, `0x4243B7`),
- every flame cell: level 1000 (`0x426E1E`, `0x42702F`),
- the closing walls, for the next value-910 (15) cells ahead of the cursor, at a level that falls by 10 per cell of distance (`0x4269EF`-`0x4269FD`).

"Safe to walk" (`0x40A59D`) means: no bomb, blank tile, no flame, danger 0.

## Behaviours, in priority order

| # | Address | Behaviour | Details |
|---|---|---|---|
| 1 | `0x40BD44` | Grab | Only with the grab powerup. If carrying a bomb: release button 1 (throw). Else, standing on an own bomb: 1 chance in 2 to press button 1 (pick up) |
| 2 | `0x40BE02` | Punch | Only with punch. 1 tick in 4: if a neighbouring cell holds a bomb, face it and press button 2 |
| 3 | `0x40B20F` | Avoid danger | Own cell safe: with trigger and no punch, 1 in 10 to press button 2 (detonate); then, if at least one neighbour is safe to walk, let later behaviours act; if none is, stand still. Own cell dangerous: find a route to a cell with lower danger (`0x40970B`), remember it, and walk the first step; on later ticks follow a path to the remembered cell (`0x4092A1`, depth 20). A step into a flame is cancelled (`0x40A76E`) |
| 4 | `0x40AD8D` | Blast bricks | With a spare bomb and not under the "cannot drop" disease: if any neighbouring cell is a brick and the own cell is free, 1 in value 915 (5) to drop a bomb |
| 5 | `0x40ABED` | Attack | With a spare bomb: if another player (other team in team play) is in the own cell or one of four neighbouring cells and the own cell is free, 1 in 5 to drop a bomb. A first condition compares a Manhattan distance with 2; its operands were not recovered |
| 6 | `0x40BAF5` | Fetch powerup | 1 tick in 50, look for the nearest collectable powerup (`0x409C1F`) within value 920 (4) + 1 steps; pursue it for up to 10 frames (500 ms), stepping only into safe cells |
| 7 | `0x40B8C2` | Hunt | 1 tick in 50, choose a living enemy (`0x422718`: random start index; human-controlled players are preferred over AIs); walk toward it by path search (depth 20), stepping only into safe cells; after 10 frames, 1 in 50 per tick to give up |
| 8 | `0x40A81F` | Wander | Keep walking in the current wander direction while the next cell is safe; 1 in 25 to turn left or right when the way ahead is open; when blocked, choose a random direction |

Note the table order in memory is 1, 2, 3, 4 (`0x40AD8D`), 5 (`0x40ABED`), 6, 7, 8.

The AI does **not** check for an escape route before dropping a bomb; it relies on behaviour 3 on the following ticks.

## Not read

- Nothing among the search routines: `0x4092A1` (path to a cell), `0x40970B` (route to safety) and `0x409C1F` (nearest powerup) were all read: see below.
- AI use of kick, spooge and jelly, if any (no behaviour for them is in the table).

## Path search (`0x4092A1`)

Confidence: MEDIUM-HIGH.

A flood of up to 100 "walkers" over a copy of the passability grid. One walker starts in each open neighbour of the start cell (in the order north, east, south, west) and remembers which first step it stands for. On every pass each walker marks its cell as used, spawns a walker to its left and to its right (which of the two first is decided once per search by a coin flip), then steps forward; a walker whose way is blocked or already used disappears. The search ends when a walker reaches the target and returns that walker's first step, or when the depth limit is reached.

Because every walker advances one cell per pass, this finds a shortest route, as a breadth-first search does. Among equally short routes the winner depends on walker order and on the coin flip.

## Route to safety (`0x40970B`)

Confidence: HIGH (decompiled in full).

The same walker flood as the path search, with depth limit 20 (`mov ebx,0x14` at `0x40B38A`), over the passability grid (blank tile, no bomb; flames do not block). There is no target: every cell a walker reaches or looks into is compared with the best danger found so far, which starts at 10000. A cell with danger 0 ends the search at once. Otherwise, when the walkers run out or the depth limit is reached, the least dangerous cell seen is the result. The start cell is never a candidate, so the result can be more dangerous than staying put. Outputs: first step (direction + 1, 0 = none), the chosen cell, passes used, peak walker count.

## Nearest powerup (`0x409C1F`)

Confidence: HIGH for the structure (decompiled in full), MEDIUM for the exact reach.

The same walker flood again, with depth limit value 920 (4) passed in EBX (`0x40BB3A`). There is no danger test and no target cell: the search ends at the first walker position where `Powerup_FindAtCell` finds a powerup and returns that walker's first step and the powerup. Nearest therefore means nearest by walking distance over blank, bomb-free cells, not by straight-line distance. With walkers starting one step away and passes 0…limit, cells up to limit + 1 steps away are reached.

## Modern implementation (`src/game/ai.*`)

The same eight behaviours in the same order with the same probabilities, as an input source separate from the core. Differences, all in the unread parts:

- Route to safety follows the original's rule (first danger-free cell within 20 steps, else the least dangerous cell seen).
- Nearest powerup is by walking distance, as in the original (it was by straight-line distance, then checked for a path).
- Searches are plain breadth-first searches over unblocked cells. For the path search this gives routes of the same length as the original's walker flood; the choice among equally short routes can differ.
- Bomb, flame and closing-wall danger levels follow the original (walls: the next 15 cells on the spiral, 250 falling by 10 per cell).
- The attack behaviour's first distance condition is omitted.

Measured behaviour of the modern AI (four AIs, standard pillar scheme at 90 % bricks, 200 rounds): 149 rounds with a single survivor, 47 draws by simultaneous deaths, 4 by time; mean round length 78.5 s; 309 of 643 deaths were by the player's own bomb. No equivalent statistics have been taken from the original yet, so how close this is in *playing strength* is unknown.
