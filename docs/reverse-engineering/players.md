# Players

Static analysis (Level 1). Sources: `Players_InitRound` (`0x4214BC`), `Players_Update` (`0x420F07`), `Player_Update` (`0x41F29B`, 6.8 KB, about half read), `Player_MoveSteps` (`0x41EC84`), `Player_DropBomb` (`0x41EB13`), `Player_Kill` (`0x41DE63`).
Field offsets refer to `structures.md`.

## Slots

10 player slots. Each has a controller type: off, local human (keyboard or joystick), AI, or network. Confidence: HIGH.

## Round start (`Players_InitRound`)

Confidence: HIGH.

- Position: centre of the start cell from the scheme (negative coordinates wrap from the right/bottom; out-of-range values clamp to the last column/row).
- Facing: south (2).
- Fuse: value 41 (40 frames). Speed: value 42 (923).
- Inventory `[0..14]` = values 50…64 (1 bomb, flame length 2, everything else 0).
- If a scheme-related lookup (`0x403A9C`) returns a powerup id 0…14, the local player (or local team) gets +1 of it (MEDIUM; this looks like the scheme's "born with" rule but only for one powerup: not fully read).
- Two shared timers are set: `0x4621E0 = value 30 × 50 ms` and `0x4621E8 = value 32 × 50 ms`. While `0x4621E0` is non-zero no player accepts input, i.e. a **1 second start freeze** (20 × 50 ms). `0x4621E8` (2 s) selects which colour index players are drawn with: this is the "show true colours before team colours" delay from value 32 (MEDIUM).

## Per-tick update order (`Player_Update`)

Confidence: MEDIUM-HIGH for the part read.

1. Slot off → return.
2. First tick: clear the area around the start cell (`0x42583B`, not read).
3. Dead with lives left → respawn at the spawn position. In campaign mode this also grants 20 frames (1000 ms) of invulnerability.
4. Dying → play the death animation, then mark dead. Death animation 9 floats upward by value 106 pixels per frame.
5. Count the player as alive (per team in team play).
6. Tick down invulnerability.
7. **Flame check** on the current cell → `Player_Kill`.
8. **Powerup pickup** if a collectable powerup is in the current cell (`Player_PickupPowerup 0x41E21E`).
9. Disease timers; disease spreading (below).
10. Trapped check: if none of the four neighbouring cells is passable, start a random "cornerhead" animation (value 330 = 13 variants); cancel it when a neighbour opens.
11. Read input unless stunned, in certain action states, or during the start freeze: AI (`ai.c`) or human (`0x41E61E`).
12. Apply reversed controls, the ice input delay, speed modifiers, conveyors; add to the movement accumulator; run `Player_MoveSteps`.
13. Choose the animation (`stand`, `walk`, `standbomb`, `walkbomb`, `kick`, `punch`, `pickup`, `cornerhead`) and queue the sprite.

14. Action buttons (see "Actions" below).
15. In a network game, send this player's position, state, direction and frame.

The whole function has now been read (session 3).

## Speed

Confidence: HIGH for the formula, MEDIUM for which disease is which.

```text
speed = base (923)
      + skates × value 90 (150)
      − inventory[13] × value 91 (150)        # roulette "clogs" (HYPOTHESIS for the slot)
if disease flag +0x84:           speed = speed / 3
if disease flag +0x85 or +0x89:  speed = speed × 3 / 2
acc += speed × frame_ms / 50
on a conveyor: add its speed if the player moves with it, subtract if against it
```

One pixel per 100 accumulated. At the default speed that is 9.23 px per 50 ms ≈ **184.6 px/s** ≈ 4.6 cells/s horizontally, 5.1 cells/s vertically (cells are 40 × 36). Skates are capped at 4 (value 554).

## Movement and collision (`Player_MoveSteps`)

Confidence: HIGH (fully read). This is the core "physics" of the game.

A cell is **passable** when its tile is blank and it contains no resting or sliding bomb (`Player_IsCellPassable 0x41E5C3`). Flying and held bombs do not block.

For each pixel step, with `d` the requested direction, `fwd` the player's offset from the cell centre along `d` (negative = before the centre), and `side` the offset perpendicular to `d`:

```text
if fwd < 0, or the next cell in direction d is passable:
    move 1 px in d
    if side != 0: also move 1 px toward the centre line   (diagonal step: lane alignment)
                  and the facing direction becomes that sideways direction
else (at or past the centre, next cell blocked):
    if side < 0: let s = the perpendicular direction on that side
                 if the neighbour cell in s AND the cell diagonally ahead of it are passable:
                     move 1 px in s                         (slide around the corner)
    if side > 0: same on the other side
    if side == 0 and fwd > 0: move back fwd pixels to the centre (in one step)
    otherwise: no movement
```

Notes:

- There is no bounding box. A player is a point; walls are enforced by never letting the point pass a cell centre toward a blocked cell.
- While moving along a corridor off-centre sideways, each step is diagonal until aligned. This is the classic Bomberman "corner assist".
- A player standing on a bomb can leave it (the test only concerns the *next* cell), and can walk up to the centre of their own cell next to a bomb.
- After every pixel: flame check (death) and powerup pickup for the new cell.
- With no direction requested, the accumulator is still consumed but nothing moves.

Special cases inside the step:

- **Kick**: if `fwd == 0`, the player has a kicker, the next cell has a bomb and the cell after it is passable → the bomb is kicked (`0x424708`) and the kick animation starts.
- **Extras**, checked when `fwd == −1` (one pixel before the centre): a warp sends the player to its linked warp (action state 6); a trampoline launches the player (action state 5; values 680/681: 30 frames, 35 px per frame).

## Input modifiers

- **Reversed controls**: disease flag `+0x8C` adds 2 to the direction (not applied to AI). MEDIUM.
- **Ice delay**: human input is pushed into a 30-entry history of `{age, direction}`; the direction used is the newest entry at least `value(450 + level)` ms old. Only the hockey rink level has a non-zero value (250 ms). MEDIUM-HIGH.

## Input (`Player_ReadInput 0x41E61E`)

Confidence: HIGH.

Controller types (`OBJ+0x10`): 0 off, 1 AI, 2 keyboard, 3 joystick, 4 network.

- **Keyboard**: two key sets (selected by `OBJ+0x11`), six keys each (four directions, button 1, button 2), read from a key-state table at `0x4A2BA0` indexed by key code.
- **Joystick**: axes are on a 0-100 scale; below 30 or above 70 counts as a direction. Buttons are bits 0 and 1.
- **Network**: position, direction and animation frame are copied from the last received packet; no local simulation of movement.

Direction resolution when several directions are held:

1. If at least one held direction leads to a passable neighbouring cell, held directions that lead to blocked cells are dropped.
2. Of what remains, the highest-numbered direction wins: west, then south, then east, then north.

There is no "last key pressed wins" rule.

## Actions

Confidence: HIGH (second half of `Player_Update`). Buttons are edge-triggered: an action fires on the tick the button goes from released to pressed.

**Button 1**, unless disease "cannot drop bombs" is active, tries in order:

1. **Grab** (needs grab): if one of the player's own bombs is in the player's cell, pick it up. The bomb is held while button 1 stays down and is thrown (as a flying bomb, speed value 301) in the facing direction when button 1 is released.
2. **Spooge** (needs spooge): if one of the player's own bombs is in the player's cell, lay bombs in a straight line in the facing direction, one per cell, until a cell contains a player or a powerup, is not passable, or the player's active bomb count reaches capacity. Each successive bomb gets one more frame of extra fuse delay.
3. **Drop**: if active bombs < capacity, the player's cell is passable (blank, no bomb) and holds no warp, create a bomb at the centre of the player's current cell.

"Active bombs" is counted by scanning all bombs for this owner (`Bombs_CountOwnedBy 0x4245DA`). Because chain detonation rewrites a bomb's owner, a bomb caught in someone else's chain stops counting against its original owner one tick before it explodes.

A bomb can be dropped at any pixel offset; it always appears at the cell centre. The player's cell is decided by the position point, so the bomb can appear slightly ahead of or behind the sprite.

**Button 2** fires all that apply:

- Kicker: every own sliding bomb (except jelly) is flagged to stop at the next cell centre.
- Punch, if button 1 is not held: a bomb in the next cell in the facing direction is launched as a flying bomb; the punch animation plays whether or not a bomb was there.
- Trigger: the player's oldest resting or sliding trigger bomb is queued for detonation.

Diseases 3 and 5 force button 1 to register as a fresh press every tick, which also releases a held bomb immediately.

## Special movement states

| State | Behaviour |
|---|---|
| Stunned (hit on head) | 16 ticks without input, pickup animation |
| Trampoline | Lasts value 680 (30) frames; drawn value 681 (35) px higher per frame up to the midpoint, then lower. At the midpoint the player is moved to a random blank, bomb-free cell within ±2 columns and ±2 rows, differing in both column and row. Cannot be killed meanwhile |
| Warp | 9 frames "spin" out, jump to the linked warp's cell centre, 9 frames in. Cannot be killed meanwhile |
| Trapped | If none of the 4 neighbouring cells is passable, a random "cornerhead" animation plays; it is cancelled when a neighbour opens |

## Diseases

Confidence: MEDIUM.

- A diseased player (`+0x78 != 0`) has a timer that ends the disease after its duration (values 130-138: 300 each; unit not confirmed).
- **Spreading**: a diseased player whose pass-cooldown is 0 infects any other living, healthy player within `|dx| ≤ cell width − 10` (30 px) and `|dy| ≤ cell height − 10` (26 px). This is a pixel-distance test, unlike the flame test. The target gets a cooldown of value 129 (10) ticks. If the "diseases multiply" option (value 123) is off, the disease is handed over instead of copied.

## Death

`Player_Kill` picks a random death animation (1…24) and reports it to the network. Refusal conditions are listed in `explosions.md`.

## Round result

`Players_Update` counts living players (and living teams) every tick; `Players_GetContendersLeft` (`0x421969`) returns living teams in team play, living players otherwise, and a constant 2 in campaign mode. `Match_Run` ends the round when it is ≤ 1.
