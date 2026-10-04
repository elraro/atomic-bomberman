# Bombs

Static analysis (Level 1). Sources: `Bomb_Create` (`0x422EDE`), `Bombs_Update` (`0x42331C`), `Player_DropBomb` (`0x41EB13`), `valuelst.res`.
Field offsets refer to `structures.md`.

## Storage

100 bomb slots (heap array at `0x46220C`). `Bomb_FindAtCell` (`0x422E48`) returns the first active bomb whose pixel position maps to the cell, **ignoring bombs that are flying or held** (motion mode 2 or 3). Confidence: HIGH.

## Creation

Confidence: HIGH unless noted.

`Player_DropBomb` passes to `Bomb_Create`:

| Property | Value |
|---|---|
| Owner | the player's index |
| Type | Starts as 0 regular; set to 2 jelly if the player has jelly; then overwritten with 1 trigger if the player has trigger and `OBJ+0x55` (trigger bombs laid since pickup) is below bomb capacity, incrementing that counter (`0x41EB2A`-`0x41EB62`) |
| Range | inventory flame length; forced to **1** with disease flag `+0x88`; forced to **max(grid width, height) = 15** with goldflame |
| Fuse | player's fuse (value 41 = 40 frames); divided by **3** with disease flag `+0x8A` |
| Position | centre of the target cell |

`Bomb_Create` then sets:

- `fuse_ms = fuse_frames × 50` → 2000 ms by default.
- `elapsed_ms = −delay_frames × 50`. A non-zero delay is used when several bombs are laid at once (spooge), so they go off in sequence (MEDIUM).
- **Dud**: in a non-network game, for a regular bomb, once a global timer has passed (`0x464AF4`, values 320/321: 180 s + up to 180 s), there is a 1-in-N chance (value 322 = 3) that the bomb becomes a dud (state 2). A dud's fuse does not run until its dud animation has played for a number of frames (values 323/324), after which it becomes a normal bomb with its animation reset (MEDIUM).

The precondition for dropping (bomb count vs. capacity, cell free) lives in the caller of `Player_DropBomb` and has not been read (UNKNOWN-016).

## Fuse

Confidence: HIGH.

Each tick, `elapsed_ms += frame_ms` **only if all** of these hold:

- more than one contender is alive (`Players_GetContendersLeft > 1`), so bombs freeze when the round is decided,
- the bomb is not a dud,
- it is not flying (mode 2) and not held (mode 3),
- it is not a trigger bomb (type 1),
- it was created locally (`+0x10 == 9`); bombs created from the network wait for a detonation message.

A sliding (kicked) bomb keeps counting.

The bomb detonates when `elapsed_ms >= fuse_ms`.

## Detonation

Confidence: HIGH. See `explosions.md` for propagation.

Chain reactions are **not immediate**:

1. When a blast reaches a cell containing a bomb, `Bomb_QueueDetonation` (`0x423209`) appends the bomb and the arrival direction to a queue (max 100). The victim's owner field is overwritten with the owner of the bomb that triggered it, so the kill credit follows the chain.
2. At the start of the next tick's bomb update (detected by a change of the tick counter), every queued bomb gets `elapsed_ms = fuse_ms` and its arrival direction stored.
3. Those bombs then detonate during that tick's normal pass, and do not send flame back in the arrival direction.

So each link in a chain costs one tick (variable real time: 1 frame of the machine).

Other callers of `Bomb_QueueDetonation`: a sliding bomb that runs into a flame, a flying bomb that lands on a flame, and the closing walls (when the "detonate" option is on).

## Movement

Bombs use the same pixel-step accumulator as players (`game-loop.md`).

### Sliding (kicked), mode 1

Confidence: MEDIUM-HIGH.

- Speed: value 300 = 1000 (10 px per 50 ms).
- Before each pixel step, at a cell centre: an arrow extra in the cell redirects the bomb.
- If the next cell holds a flame: the bomb is queued for detonation.
- If the next cell is not passable (solid, brick, bomb, …; test is `0x4230A5`, not read) and the bomb has reached the centre: it snaps to the cell centre and stops (mode 0). **Jelly bombs** instead reverse direction and keep sliding.
- Kick trigger (in `Player_MoveSteps`): the player has a kicker, stands exactly on the cell centre line along the facing direction, the next cell holds a bomb, and the cell beyond it is passable.

### Flying (punched / thrown), mode 2

Confidence: MEDIUM.

- Speed: value 301 = 1300.
- The bomb travels over everything. Each time it reaches a cell centre:
  - wraps around the playfield when it goes more than one cell outside (the wrap distance is grid size + 3 cells),
  - the first hop continues until 3 cell centres have been passed; later hops are one cell (values 660/661 give the arc heights 65 and 20 px, drawn with a sine),
  - it lands only if the cell is blank and holds no bomb, no powerup and no warp. Landing on a player's cell stuns the player (`0x421F7E`; values 670/671: lose 1 + up to 3 powerups) and the bomb bounces on,
  - a jelly bomb inside the grid has a 1-in-N chance (value 667 = 3) of turning left or right at each bounce,
  - on landing it stops (mode 0); if a flame is there it is queued for detonation.
- The fuse does not run while flying.

### Held, mode 3

Updated after players. The bomb is positioned relative to its holder; if the holder pointer is null it drops to the cell centre and rests (MEDIUM).

### Conveyors, mode 0

A resting bomb on a conveyor extra moves at the conveyor speed (values 190-192) and is pulled toward the cell centre line (MEDIUM).

## AI danger marking

While a bomb exists, every blank cell within its range in the four directions (stopping at solid/brick, bombs and powerups) is passed to `0x424DFE` each tick. This is believed to be the AI's danger map (HYPOTHESIS, function not read).

## Edge cases asked for in AGENTS.md §20

| Case | Finding | Confidence |
|---|---|---|
| Bomb next to solid wall | Blast stops before the wall | HIGH |
| Bomb next to brick | Brick burns; blast stops there | HIGH |
| Bomb next to another bomb | Second bomb detonates on the next tick, no flame returns toward the first | HIGH |
| Multiple bombs with the same fuse time | Processed in slot order within one tick; each lays its own flames | HIGH |
| Chain timing | One tick per link | HIGH |
| Bomb placed while moving | Not yet traced (UNKNOWN-016) | – |
| Player trapped by bomb | A bomb's cell is impassable to enter; a player standing on it can walk off (movement is only blocked when entering the *next* cell). Being enclosed on all four sides starts a "cornerhead" animation | MEDIUM |
