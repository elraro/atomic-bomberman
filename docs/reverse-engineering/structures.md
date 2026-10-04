# Structures

Evidence only. Do not copy these layouts into the modern code (AGENTS.md §41).
All findings are static (Level 1). Offsets are in bytes.

## OBJ (152 bytes, 0x98)

Confidence that one structure is shared by players, bombs, flames (and probably powerups and extras): HIGH. The same offsets are used with the same meaning by `Player_Update`, `Bombs_Update`, `Flames_Update`, and all three arrays use a 0x98 stride. The game logs `sizeof(OBJ) = 152`.

Instances:

| Kind | Storage | Count | Evidence |
|---|---|---|---|
| Players | static array at `0x461BC4` | 10 | `Players_Update`, `Player_FindAtCell` |
| Bombs | heap, pointer at `0x46220C` | 100 | `Bombs_Update`, `Bomb_FindAtCell` |
| Flames | heap, pointer at `0x46224C` | 15 × 11, one per cell, index `y*15 + x` | `Flame_Create`, `Flame_FindAtCell` |
| Powerups | heap, pointer at `0x462214` | 15 × 11, one per cell | `Powerups_GenerateForRound`, `Powerup_RevealAtCell` |
| Extras | found through `Extra_FindAtCell` (`0x405654`) | not read yet | |

### Fields common to all kinds

| Offset | Type | Meaning | Confidence |
|---|---|---|---|
| `+0x00` | i32 | State. 0 = free/dead, 1 = active. Bombs: 2 = dud. Flames: −1 = remove after this draw. Powerups: 2 = collectable | HIGH |
| `+0x04` | i32 | Subtype. Bombs: 0 regular, 1 trigger, 2 jelly. Flames: 9 = burning-brick flame, otherwise the flame piece. Players: death animation number (9 = angel floating up). Extras: 0 arrow, 1 warp, 2 conveyor, 3 trampoline | HIGH (bombs, flames), MEDIUM (others) |
| `+0x10` | u8 | Controller. Players: 0 off, 1 AI, 2 keyboard, 3 joystick, 4 network. Bombs: 9 = created locally, 4 = created from network | HIGH |
| `+0x11` | u8 | Players: keyboard set (0 or 1) or joystick number | MEDIUM |
| `+0x1C` | i32 | X position in screen pixels (flames: cell X) | HIGH |
| `+0x20` | i32 | Y position in screen pixels (flames: cell Y) | HIGH |
| `+0x2C` | i16 | Direction 0 north, 1 east, 2 south, 3 west. The code reads the dword at `+0x2A` and shifts right 16 to sign-extend it | HIGH |
| `+0x2E` | i16 | Players: requested direction this tick (−1 = none). Bombs: motion mode 0 resting, 1 sliding (kicked), 2 flying (punched/thrown), 3 held | HIGH |
| `+0x30` | i16 | Animation frame counter (advances once per 50 ms) | HIGH |
| `+0x32` | i16 | Millisecond accumulator for `+0x30` | HIGH |
| `+0x3C` | u8 | Colour/remap index (player number, or team colour in team play) | MEDIUM |
| `+0x3E` | i16 | Owner player index (bombs, flames: who gets the kill) | HIGH |
| `+0x44` | i16 | Elapsed milliseconds (bomb fuse elapsed, flame age) | HIGH |
| `+0x70` | i32 | Speed, 1/100 pixel per 50 ms | HIGH |
| `+0x74` | i32 | Movement accumulator: one pixel step per 100 units | HIGH |
| `+0x94` | ptr | Bombs: player holding it. Players: bomb being held | MEDIUM |

### Player-only fields

| Offset | Type | Meaning | Confidence |
|---|---|---|---|
| `+0x08` | i32 | Dying (death animation in progress) | HIGH |
| `+0x14`, `+0x18` | i32 | Spawn position in pixels (respawn copies them into `+0x1C/+0x20`; also rewritten when warping) | MEDIUM |
| `+0x24`, `+0x28` | i32 | Pixel delta applied in the current movement step | HIGH |
| `+0x36`, `+0x37` | u8 | Previous-tick copies of `+0x38`, `+0x39` (for edge detection) | HIGH |
| `+0x38`, `+0x39` | u8 | Button 1 (bomb) and button 2 (action) held this tick | HIGH |
| `+0x3A` | i16 | Stun counter in **ticks** (blocks input while > 0) | HIGH |
| `+0x4A` | i16 | Fuse length in frames given to this player's bombs (from value 41) | HIGH |
| `+0x4E` | i16 | Action state: 0 none, 1 kicking, 2 punching, 3 picking up, 4 holding, 5 trampoline, 6 warping, 7 unknown, 20+ = "cornerhead" trapped animation | MEDIUM |
| `+0x50`, `+0x52` | i16 | Action animation frame and its ms accumulator | MEDIUM |
| `+0x54` | u8 | Team | MEDIUM |
| `+0x55` | u8 | Trigger bombs laid since the trigger powerup was picked up (reset to 0 on pickup). Trigger bombs stop being produced once it reaches bomb capacity. Not the active-bomb count: that is computed by scanning the bomb array | HIGH |
| `+0x56` … `+0x64` | u8[15] | Inventory, indexed by powerup id (initialised from values 50-64): 0 bombs, 1 flame length, 2 disease, 3 kicker, 4 skates, 5 punch, 6 grab, 7 spooge, 8 goldflame, 9 trigger, 10 jelly, 11 super disease, 12 random, 13 clog (speed-down), 14 unused | HIGH |
| `+0x66` | i16 | Invulnerability time, ms | MEDIUM |
| `+0x68` | u8 | Lives remaining (init 1; decremented on respawn) | MEDIUM |
| `+0x6A` | i16 | Shown in the top-of-screen player panel (probably round wins) | LOW |
| `+0x6C` | i32 | Kill score: +1 per kill, −1 per suicide (`0x41DCB2`) | HIGH |
| `+0x78` | i32 | Disease elapsed ms (0 = healthy) | MEDIUM |
| `+0x7C` | i32 | Disease duration | MEDIUM |
| `+0x80` | i32 | Ticks before the disease can be passed on again | MEDIUM |
| `+0x84` … `+0x91` | u8[14] | Active disease flags, index = disease number (9 are used; see `powerups.md`) | HIGH |
| `+0x92` | u8 | "Start cell already cleared" flag | LOW |

### Bomb-only fields

| Offset | Type | Meaning | Confidence |
|---|---|---|---|
| `+0x0C` | i32 | Network bomb id | MEDIUM |
| `+0x38` | u8 | Direction + 1 from which a chain detonation arrived (0 = own fuse). The blast does not propagate back in that direction | HIGH |
| `+0x39` | u8 | Sliding bomb: "stop at next cell centre" flag | MEDIUM |
| `+0x40` | i32 | Tick counter value at creation | MEDIUM |
| `+0x46` | i16 | Pixels travelled in the current flight hop | MEDIUM |
| `+0x48` | i16 | Number of cell centres passed while flying (the first hop lasts until 3) | MEDIUM |
| `+0x4A` | i16 | Fuse length in ms (= frames × 50) | HIGH |
| `+0x4C` | u8 | Blast range in cells | HIGH |

## Map

| Global | Value | Meaning | Confidence |
|---|---|---|---|
| `0x4648AC` | 15 | Grid width | HIGH |
| `0x4648B4` | 11 | Grid height | HIGH |
| `0x4648A4` | 40 | Cell width, pixels | HIGH |
| `0x4648A0` | 36 | Cell height, pixels | HIGH |
| `0x464898` | 20 | Grid origin X = (640 − 15·40) / 2 | HIGH |
| `0x4648A8` | 68 | Grid origin Y = 480 − 11·36 − 16 | HIGH |
| `0x46222C` | ptr | `int tiles[11][15]`: 0 blank, 1 solid, 2 brick. Out-of-range reads return 1 | HIGH |

## Other tables

| Address | Content |
|---|---|
| `0x45BECC` | `int dx[4] = {0, 1, 0, −1}` |
| `0x45BEDC` | `int dy[4] = {−1, 0, 1, 0}` |
| `0x4621F8`, `0x4621FC`, `0x462200` | Pending-detonation queue: bomb pointers, arrival directions, count (max 100) |
| `0x4621C8` | Per-player input history: 30 × `{age_ms, direction}` used for the ice control delay |
| `0x46460C`, `0x46465C` | Start cell X and Y per player (from the scheme) |
| `0x4A37A8` | GNW background-process list: nodes of `{flags, fn, next}` |
