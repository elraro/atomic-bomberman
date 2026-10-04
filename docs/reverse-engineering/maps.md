# Maps

Static analysis (Level 1) plus first-party data files.

## Grid

Confidence: HIGH (constants written at `0x426488`–`0x4264EB`, map init in `map.c`).

| Property | Value |
|---|---|
| Grid | 15 columns × 11 rows |
| Cell size | 40 × 36 pixels |
| Playfield origin | x = 20, y = 68 on a 640 × 480 screen |
| Tile values | 0 blank, 1 solid, 2 brick |
| Outside the grid | treated as solid (`Map_GetTile 0x425FB9`) |

There is **no border of solid tiles in the data**: the 15 × 11 scheme is the whole playfield, and the edge is enforced by the out-of-range rule.

## Coordinates

Object positions are integer screen pixels. An object's reference point is the horizontal centre and near the bottom of its cell.

| Function | Formula |
|---|---|
| `Map_CellToPixelX 0x426524` | `20 + 20 + cx·40` |
| `Map_CellToPixelY 0x42655F` | `68 + 35 + cy·36` |
| `Map_PixelToCellX 0x42665C` | `(px − 20) / 40` |
| `Map_PixelToCellY 0x4266A3` | `(py − 68 − 17) / 36` |
| `Map_PixelOffsetInCellX 0x426599` | `(px − 20) mod 40 − 20`, range −20…19, 0 at the cell centre |
| `Map_PixelOffsetInCellY 0x4265EB` | `(py − 68 − 17) mod 36 − 18`, range −18…17, 0 at the cell anchor |

Negative inputs are handled specially in the cell functions (objects can be off-grid while flying); the exact rounding there has not been checked (LOW).

## Scheme files (`data/schemes/*.sch`)

Confidence: HIGH (text format, matches the writer strings in `scheme.c`).

```text
-V,2                         version
-N,<name>
-B,<0-100>                   brick density percent
-R,<row 0-10>,<15 chars>     '#' solid, ':' brick, '.' blank
-S,<player 0-9>,<x>,<y>,<team>
-P,<powerup 0-12>,<bornwith>,<has_override>,<override_value>,<forbidden>,<comment>
```

### Round generation

Confidence: HIGH (`Map_GenerateForRound 0x4260F5`, comparison at `0x4262FF`-`0x426318`).

- `#` → solid, `.` → blank.
- Each `:` cell independently stays a brick if `rand() % 100 < density`, otherwise it becomes blank. Density 100 keeps every brick.
- On each player's first update, the start cell and its four neighbours are cleared, except neighbours that are solid (`Map_ClearStartArea 0x42583B`).
- Powerups are then hidden under bricks (`powerups.md`).
- A network client receives the tiles from the host instead of generating them.

Default start cells when a scheme gives none: `valuelst.res` 600-618 (negative values count from the right/bottom edge; `Players_InitRound` wraps and clamps them).

## Levels (themes)

11 levels (`valuelst.res` 35), each with `fieldN.pcx` background, `tilesN.ani`, `xbrickN.ani` and optionally `extraN.res`. Levels are independent of schemes. Random selection skips levels disabled in values 1150-1161.

`extraN.res` records (from `extra.c` error strings): `-A,dir,X,Y` arrow, `-W,type,id,X,Y,linkto` warp hole, `-C,dir,X,Y` conveyor, `-T,X,Y` trampoline.

## Enclosement ("hurry" closing walls)

Function: `Map_UpdateEnclosement` (`0x426818`). Confidence: MEDIUM-HIGH for the mechanism, LOW for the exact start condition.

- Only runs while more than one contender is alive.
- Before the closing phase, the same function drives **tile regeneration** (`0x426704`) on levels where value `340 + level` is non-zero (only level 7, every 4 s), except on a network client.
- The closing phase is armed when `time_left <= value 101 − 5`, i.e. at **55 seconds left** by default (disassembly `0x42685D`-`0x426873`, HIGH). It never starts with an infinite timer.
- A cursor starts at cell (0,0) heading east and walks an inward clockwise spiral. Every **250 ms of wall-clock time** (`timeGetTime`, not `frame_ms`), at most 4 per tick, the cursor cell is processed and the cursor advances:
  - the tile becomes solid,
  - any player in the cell is killed (network-controlled players are skipped),
  - powerups in the cell are removed,
  - a bomb in the cell is detonated or removed, depending on the option initialised from value 46,
  - a flame in the cell is removed.
- The spiral stops after `2 × enclosement_depth` rings (option `enclosement_depth`, default value 27 = 1).
- The next value-910 (15) cells ahead of the cursor are marked as dangerous for the AI each tick.

Because the 250 ms cadence uses real time, this is the one mechanic found so far that is not driven by the clamped `frame_ms`.
