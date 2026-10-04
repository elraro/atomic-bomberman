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

- every bomb, for each cell its blast would reach (`0x4242DE`, `0x4243B7`; the level passed is a computed value that was **not recovered**: UNKNOWN-023),
- every flame cell: level 1000 (`0x426E1E`, `0x42702F`),
- the closing walls, for the next value-910 (15) cells ahead of the cursor.

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

- The three search routines (`0x4092A1` path to a cell, `0x40970B` route to safety, `0x409C1F` nearest powerup): their expansion order and tie-breaking.
- The bomb danger level formula.
- AI use of kick, spooge and jelly, if any (no behaviour for them is in the table).

## Modern implementation (`src/game/ai.*`)

The same eight behaviours in the same order with the same probabilities, as an input source separate from the core. Differences, all in the unread parts:

- Searches are plain breadth-first searches over unblocked cells.
- A bomb's danger level is `1 + elapsed fuse in frames`; flames are 1000; the closing walls are not marked.
- The attack behaviour's first distance condition is omitted.

Measured behaviour of the modern AI (four AIs, standard pillar scheme at 90 % bricks, 200 rounds): 149 rounds with a single survivor, 47 draws by simultaneous deaths, 4 by time; mean round length 78.5 s; 309 of 643 deaths were by the player's own bomb. No equivalent statistics have been taken from the original yet, so how close this is in *playing strength* is unknown.
