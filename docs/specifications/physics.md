# Physics: position, movement, collision

See `README.md` for tags.

## Position

- Every player and bomb has an integer pixel position. [S]
- The position is a single point. There are no bounding boxes. [S]
- The **cell** of a point (x, y) is `((x − 20) div 40, (y − 85) div 36)` in original screen pixels. [S]
- The **centre** of cell (cx, cy) is the point `(40 + 40·cx, 103 + 36·cy)`. [S]
- The **offset** of a point within its cell is its distance from that centre: −20…+19 horizontally, −18…+17 vertically. [S]

A resolution-independent implementation may use cell-relative units, but must keep 40 horizontal and 36 vertical steps per cell so that speeds in pixels per frame keep their meaning.

## Directions

0 north (y−1), 1 east (x+1), 2 south (y+1), 3 west (x−1). [S]

## Passability (players)

A cell is passable for a player when it is blank and contains no bomb that is resting or sliding. Flying and held bombs do not block. Powerups, flames and other players never block. [S]

## One movement step (players)

Let `d` be the requested direction, `fwd` the offset along `d` (negative before the centre, positive past it) and `side` the offset perpendicular to `d`. [S]

```text
A. if fwd < 0, or the next cell in direction d is passable:
       move 1 px in d
       if side != 0:
           also move 1 px toward the centre line     (the step is diagonal)
           the player now faces that sideways direction

B. otherwise (at or past the centre, next cell blocked):
       if side != 0:
           let s be the sideways direction pointing AWAY from the centre line,
           i.e. toward the neighbouring lane on the side the player is already on
           if the neighbouring cell in s is passable
              and the cell beyond it in direction d is passable:
               move 1 px in s                        (slide around the corner)
       else if fwd > 0:
           move back to the centre                   (all fwd pixels at once)
       else:
           no movement
```

Rule B was checked against the code for the eastward case (`side` = vertical offset; above the line the player slides north, below it south) and holds for the other directions by symmetry. [S]

Consequences:

- A player can always reach the centre of the current cell. [S]
- A player can never move past a cell centre toward a blocked cell. [S]
- A player standing in a cell with a bomb can walk out of it in any open direction. [S]
- Turning into a side corridor while not aligned is assisted automatically. [S]

After every single pixel moved, the player's new cell is checked for a flame (death) and for a collectable powerup (pickup). [S]

## Speed (players)

```text
speed = V(42) [923] + skates · V(90) [150] − clogs · V(91) [150]
slow disease:   speed = speed div 3
fast disease:   speed = speed · 3 div 2
```

With conveyors: add the conveyor speed (V(190..192) [250, 350, 450]) when moving along it, subtract when moving against it; a player who is not moving is carried by the conveyor. [M]

Default speed 923 gives 9.23 pixels per frame ≈ 184.6 px/s. [S]

## Bombs

Bombs move with the same stepping rule (`bombs.md`). A sliding bomb's passability test is stricter than a player's: the next cell must be blank and contain no bomb, no player and no warp hole; a collectable powerup there is destroyed and does not block. [S]
