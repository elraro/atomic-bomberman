# Bombs

See `README.md` for tags.

## Properties

Owner, type (regular, trigger, jelly), blast range in cells, fuse length, elapsed fuse time, motion mode (resting, sliding, flying, held), direction. At most 100 bombs exist at once. [S]

## Creation

| Property | Rule |
|---|---|
| Position | centre of the cell [S] |
| Range | the owner's blast range; 1 with the short-flame disease; 15 with goldflame [S] |
| Fuse | the owner's fuse V(41) [40] frames = 2000 ms; one third with the short-fuse disease [S] |
| Type | trigger if the owner has trigger **and** has laid fewer trigger bombs than their bomb capacity since picking the trigger up; otherwise jelly if the owner has jelly; otherwise regular. (Trigger and jelly exclude each other on pickup, so both can only be held through scheme settings.) [S] |
| Dud | local games only: after V(320) [180] + random(0…V(321) [180]) seconds have passed since the last dud opportunity, a regular bomb has a 1 in V(322) [3] chance of being a dud [M] |

## Fuse

Elapsed time advances by `dt` each tick only when all hold: [S]

- more than one contender is alive,
- the bomb is not a dud that is still fizzling,
- the bomb is resting or sliding (not flying, not held),
- the bomb is not a trigger bomb,
- the bomb is simulated by this machine.

The bomb detonates in the tick in which elapsed ≥ fuse.

A dud fizzles for V(323) [120] animation frames, then becomes a normal bomb whose fuse starts running. V(324) ("random additional" frames) exists in the data but no use of it was found. [M]

## Detonation and chains

- Detonation removes the bomb and creates flames (`explosions.md`). [S]
- A bomb reached by a blast, hit by the closing walls, or sliding or landing onto a flame does **not** explode immediately. It is queued and explodes during the next tick. [S]
- A chained bomb sends no flame back in the direction the triggering blast came from. [S]
- A chained bomb's owner becomes the owner of the bomb that triggered it. [S]
- At most 100 bombs can be queued per tick. [S]

## Sliding (kicked)

- Speed V(300) [1000]. [S]
- At each cell centre: an arrow tile redirects the bomb. [M]
- Before entering the next cell: [S]
  - if it contains a flame, the bomb is queued for detonation and stops at the centre of its current cell;
  - if it is free (see `physics.md`), the bomb continues;
  - otherwise the bomb stops at the centre of its current cell. A **jelly** bomb instead reverses direction and keeps sliding.
- A sliding bomb flagged to stop (owner pressed button 2 with a kicker) stops at the next cell centre. [S]
- Kicking a bomb that is already sliding in a different direction first snaps it to its cell centre. [S]
- The fuse keeps running while sliding. [S]

## Flying (punched or thrown)

- Speed V(301) [1300]; the fuse is paused. [S]
- The bomb starts from the centre of its cell and passes over everything. [S]
- The first hop is 3 cells; each later hop is 1 cell. Visual arc heights are V(660) [65] and V(661) [20] pixels. [M]
- More than one cell beyond the edge, the bomb wraps to the opposite side. [S]
- At the end of a hop the bomb **lands** if the cell is blank, has no bomb, no powerup and no warp hole, and no player. Otherwise it hops again. If a player is in the cell, that player is hit on the head (`players.md`). [S]
- A jelly bomb inside the grid has a 1 in V(667) [3] chance at each bounce of turning 90° left or right. [S]
- On landing the bomb rests; if the cell has a flame it is queued for detonation. [S]

## Held

A grabbed bomb follows its holder and its fuse is paused; its elapsed time is reset to 0 when thrown. [S]

## Conveyors

A resting bomb on a conveyor is carried at the conveyor's speed. [M]
