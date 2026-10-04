# Maps

See `README.md` for tags.

## Grid

- 15 columns × 11 rows. Column 0 is the left edge, row 0 the top. [S]
- Each cell is one of: **blank**, **solid** (indestructible), **brick** (destructible). [S]
- Everything outside the grid behaves as solid. Schemes do not contain a wall border. [S]
- On the original 640 × 480 screen a cell is 40 × 36 pixels and the grid's top-left corner is at (20, 68). [S]

## Scheme

A scheme defines: [D]

- a name,
- a brick density, 0-100,
- the 15 × 11 layout of solid / brick / blank cells,
- a start cell and a team (0 or 1) for each of 10 players,
- per powerup type: born-with count, optional override of the count on the level, and a "forbidden" flag.

Born-with, when greater than 0, replaces the starting amount of that powerup for every player. Override, when enabled, replaces the number of that powerup generated on the level (same sign convention as `powerups.md`). Forbidden excludes the type from what a "random" powerup can turn into; no other effect of it was found. [S]

## Level theme

A round also has a level theme (0-10), independent of the scheme, which selects graphics and optional extras (arrows, warp holes, conveyors, trampolines). [D] Two themes change rules: the hockey rink delays controls by V(452) [250] ms (`players.md`); the cemetery regenerates bricks every V(347) [4] seconds. [D]

## Extras

A level theme may place arrows, conveyors, warp holes and trampolines on fixed cells. They act only when their cell is open. [S]

- **Arrow**: a sliding bomb arriving at the cell centre turns to the arrow's direction. [S]
- **Conveyor**: carries resting bombs and idle players at the belt speed V(190..192) [250, 350, 450]; adds to or subtracts from a walking player's speed. [S][M]
- **Warp hole**: clears its own cell and one random neighbour at round start. A player walking onto it disappears for 9 frames, reappears on the linked warp, and is out of play for 9 more frames. [S]
- **Trampoline**: a player walking onto it is airborne for V(680) [30] frames and comes down on a random free cell up to two cells away in both axes. [S]

A player in a warp or in the air ignores input and cannot be killed. [S]

## Round generation

1. Solid and blank cells are copied from the scheme. [S]
2. Each brick cell of the scheme is kept with probability density / 100, independently; otherwise it becomes blank. [S]
3. Powerups are hidden under bricks (`powerups.md`). [S]
4. When a player first acts, their start cell and its four orthogonal neighbours become blank, except neighbours that are solid. [S]

Start cells: from the scheme, optionally shuffled among players (`random_start`). [M] Negative coordinates count from the right or bottom edge. [S]

## Closing walls

Enabled by the enclosement depth option (0-3, default V(27) [1]). [D]

- Active only while more than one contender is alive. [S]
- Begins when the time left is at most V(101) − 5 = 55 seconds. Never begins when the round has no time limit. [S]
- The phase is armed when the displayed clock reaches 55 seconds; the first cell closes 250 ms later. [S, observed]
- A cursor starts at cell (0, 0) moving east and follows a clockwise inward spiral. Every 250 ms the cursor's cell is closed and the cursor advances; at most 4 cells are processed per tick. [S, observed]
- At each turn of the spiral the corner cell is processed a second time (it uses one 250 ms slot and closes nothing new), so the top row takes 16 slots, not 15. [S, observed]
- The interval is measured in real time, not in clamped game time. [S]
- Closing a cell: [S]
  - it becomes solid;
  - a locally simulated player in it dies;
  - a powerup in it is removed;
  - a bomb in it detonates if the "stomped bombs detonate" option is on (default V(46) [1]), otherwise it is removed;
  - a flame in it is removed.
- The spiral covers `2 × depth` rings and then stops. [S]
