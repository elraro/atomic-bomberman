# Explosions (flames)

Static analysis (Level 1). Sources: the detonation block of `Bombs_Update` (`0x42331C`), `Flame_Create` (`0x426FCC`), `Flames_Update` (`0x426D06`), `Player_Update` (`0x41F29B`).

## Representation

Confidence: HIGH.

Flames are **per-cell**: one `OBJ` slot for each of the 15 × 11 cells (`0x46224C`). Creating a flame in a cell overwrites whatever flame was there (age reset to 0, new owner). There are no explosion objects that span cells.

Each flame stores: state, piece type (centre / mid / tip per direction, or 9 = burning brick), colour, owner player (−1 for a burning brick), age in ms.

## Propagation

Confidence: HIGH.

When a bomb detonates:

```text
bomb slot freed; detonation sound; (network: detonation message)
for dir in north, east, south, west:
    if dir is the direction the triggering blast came from: skip
    place a flame on the bomb's own cell          # done once per processed direction
    if a collectable powerup is on the bomb's cell: destroy it
    cell = bomb cell
    repeat `range` times:
        cell += dir
        if a bomb is in cell:        queue it for detonation (owner := this bomb's owner); stop
        if a collectable powerup:    destroy it; stop            # no flame is placed on that cell
        tile = Map_GetTile(cell)     # outside the grid counts as solid
        if tile == solid:            stop
        if tile == brick:            place burning-brick flame (owner −1); start brick destruction;
                                     reveal the powerup under it; stop
        place a flame on cell
```

Order matters: bomb, then powerup, then tile.

## Behaviour matrix

| Object in the cell | Flame placed there? | Blast continues? | Effect | Confidence |
|---|---|---|---|---|
| Blank tile | yes | yes | – | HIGH |
| Solid tile / edge of grid | no | no | – | HIGH |
| Brick | burning-brick flame | no | Brick destroyed when that flame ends; hidden powerup revealed | HIGH |
| Bomb (resting or sliding) | no | no | Detonates next tick; kill credit transfers | HIGH |
| Bomb flying or held | (not seen by the lookup) | yes | Unaffected | HIGH |
| Powerup (collectable) | no | no | Destroyed. A destroyed disease may be handled differently (`0x4255B2` is called when the powerup's type is 2 and option `0x464990` is 0) | HIGH / MEDIUM |
| Player | yes | yes | Killed when the player's cell contains a flame | HIGH |
| Existing flame | replaced | yes | Age restarts | HIGH |

## Lifetime

Confidence: MEDIUM-HIGH. The `Value_Get` ids are lost in the decompilation; values 10 and 20 are identified from their comments in `valuelst.res`.

- Regular flame: removed when `age_ms > N × 50`, N = value 10 = 10 frames → **500 ms**.
- Burning brick: lasts N = value 20 = 10 frames → **500 ms**; when it ends, a brick tile in that cell is changed (to blank) and the flame is removed after one more draw.
- Age advances by `frame_ms` every tick, unconditionally.

## Killing players

Confidence: HIGH.

The test is **cell-based**, not pixel overlap: a living, non-dying player is killed if `Flame_FindAtCell(cell of the player's position)` returns an active flame. It is evaluated

- once at the start of each player's update, and
- after **every single pixel** of movement in `Player_MoveSteps`.

`Player_Kill` (`0x41DE63`) refuses to kill when: the player is network-controlled (the owning machine decides), or the player's action state is 5, 6 or 7 (trampoline, warp, and one unknown state). Invulnerability time (`+0x66`) is counted down in `Player_Update`, but where it blocks death has not been located (UNKNOWN-017).

On death a random death animation is chosen (1…value 105 = 24) and the flame's owner is recorded as the killer.

## Burning-brick flames and players

A burning-brick flame (type 9) is still an active flame in `Flame_FindAtCell`. Players cannot normally be in a brick cell, so this only matters for edge cases (LOW; not analysed).

## Not yet known

- Piece selection (centre/mid/tip) and animation frames: the piece type is passed in a register that the decompiler lost; needs a pass over the disassembly.
- Trigger-bomb detonation path and the network "detonate" message.
