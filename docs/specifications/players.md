# Players

See `README.md` for tags.

## Slots and controllers

Up to 10 players. Each is controlled by: keyboard (two key sets), joystick, AI, or the network. [S]

## State at round start

| Property | Value |
|---|---|
| Position | centre of the start cell [S] |
| Facing | south [S] |
| Speed | V(42) [923] [S] |
| Fuse given to bombs | V(41) [40] frames [S] |
| Inventory | V(50…64): 1 bomb, blast range 2, nothing else [S] |
| Input | ignored for the first 1000 ms [S] |

## Input

Each tick a controller provides four direction flags and two buttons. [S]

Direction choice when several are held: [S]

1. If at least one held direction leads to a passable neighbouring cell, ignore the held directions that do not.
2. Among the rest, priority is west, south, east, north (highest first).

Modifiers:

- **Reversed-controls disease**: the chosen direction is turned 180°. Human players only. [S]
- **Ice**: on a level with a control delay `T` (hockey rink: 250 ms), the direction applied is the one that was chosen `T` ms ago. The original keeps the last 30 ticks of history. [S]

Buttons act on the tick they go from released to pressed. [S]

## Button 1

Ignored while the "cannot drop bombs" disease is active. Otherwise the first applicable rule fires: [S]

1. **Grab** (has grab; one of the player's own bombs is in the player's cell): the bomb is picked up and carried. When button 1 is released it is thrown in the facing direction.
2. **Spooge** (has spooge; one of the player's own bombs is in the player's cell): bombs are placed cell by cell in the facing direction until the next cell contains a player or a powerup, or is not passable, or the player has no spare capacity. The n-th bomb of the line starts with its fuse n frames (n × 50 ms) behind, so the line detonates in sequence away from the player.
3. **Drop**: if the player's active bombs are fewer than capacity, and the player's cell is passable and has no warp hole, a bomb is created at the centre of that cell.

Active bombs = bombs on the field whose owner is this player. [S]

## Button 2

All applicable rules fire: [S]

- **Kicker**: all of the player's own sliding bombs, except jelly bombs, stop at the next cell centre.
- **Punch** (only if button 1 is not held): a bomb in the next cell in the facing direction is punched (`bombs.md`).
- **Trigger**: the oldest of the player's trigger bombs that is not flying or held detonates.

## Kicking

While moving, a player with a kicker who is exactly at `fwd = 0` (see `physics.md`), facing a cell that contains a bomb, with the cell beyond that bomb free for sliding, kicks the bomb in the facing direction. [S]

## Death

- A player dies when the cell containing their position holds a flame. Checked at the start of the player's update and after each pixel moved. [S]
- A player is immune while on a trampoline or inside a warp, and while an invulnerability timer is running (given for 1000 ms on respawn in campaign mode). [S]
- Kill score: the killer gains 1; killing yourself costs 1 unless an option disables that. [S]
- Death is decided only on the machine that controls the player. [S]
- The killer is the owner recorded on the flame. [S]
- One of V(105) [24] death animations is chosen at random. [S]

## Being hit by a flying bomb

When a punched or thrown bomb lands on a player's cell: the player is stunned for 16 ticks, loses `V(670) [1] + random(0 … V(671)−1 [0…2])` powerups, chosen at random from those above the starting amounts, and the lost powerups reappear on random free blank cells. The bomb bounces on. Local games only. [S]

## Diseases

See `powerups.md`. A diseased player whose pass cooldown is zero infects any other living, healthy player whose position is within 30 px horizontally and 26 px vertically (cell size minus 10). The newly infected player cannot pass it on for V(129) [10] ticks. Whether the disease is copied or handed over is an option (V(123) [1] = copied). [S]

## Trapped

If none of the four neighbouring cells is passable, the player plays one of V(330) [13] "trapped" animations; it has no gameplay effect found so far. [S]
