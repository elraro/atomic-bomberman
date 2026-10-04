# Rendering

## Original

640 × 480, 8-bit palettised, one global palette (`color.pal`, 6-bit values shown ×4). Sprites are stored as 16-bit RGB555 frames in `.ani` files and converted at load time to palette indices through a 32 768-entry lookup table (second part of `color.pal`). Player colours are produced by remapping a band of green palette indices (100-174) through `N.rmp`; other indices are left alone (the remap table holds 0 for them). Backgrounds are 8-bit PCX files whose palette equals the global one in 254 of 256 entries.

## Modern

`src/rendering/sprites.*` (`SpriteBank`) reads the user's own game files at run time and builds one RGBA texture per (frame, player colour) on first use, following the same steps as the original: RGB555 → lookup table → optional remap → palette colour ×4. Key-coloured pixels and pixels that map to index 0 are transparent.

`src/rendering/renderer.*` draws, in order: the level background (`fieldN.pcx`), then per cell solid/brick tiles, revealed powerups and flames, then bombs, then players back to front (shadow, then `stand <dir>` or `walk <dir>`). A sprite is placed with its hot-spot on the object's reference point. Without game files, or with `--shapes`, flat placeholder shapes are drawn instead.

Sequence names used: `tile N solid|brick`, `power <name>`, `flame center|mid<dir>|tip<dir> green`, `flame brick N`, `bomb regular|trigger|jelly green`, `stand <dir>`, `walk <dir>`, `shadow`.

## Comparison with the original

Scene: open test scheme, two players at their start cells, level 0. The original's own screenshot and the modern render (`--native`, 640 × 480) agree in **99.46 %** of the playfield pixels; the player sprite coincides in position, pose and colour. The differing pixels are isolated background pixels (the two palette entries in which `field0.pcx` and `color.pal` differ).

Finding from this comparison: the per-step x/y values stored in sequence `STAT` records are **not** added to the draw position of players (applying them moved the player 19 px down, off the original's position). For **flames** the original does use them (`0x426D06` asks for the step's offsets and draws at reference + offset, half a cell higher); the modern renderer does the same, which is what makes the centre piece and the arms line up. This was reported from play-testing: before the fix the arms were drawn about 7 px below the centre.

## Known gaps

- The HUD is drawn with the original assets: clock digits from `kfont.ani` at the original position, the blinking `hurry` banner, and `S:n K:n` score labels in `font1.fon` at the original coordinates (compared with an original screenshot: clock and first label coincide; the black player's outlined label is approximated). Pickup, warp and trapped ("cornerhead") animations are not drawn.
- Death (`die green N`), kick, punch and carrying (`walkbomb`/`standbomb`) animations are drawn; the level theme is selectable (`--level`). None of these has been compared frame by frame with the original.
- The walk animation uses the original's rule (advance one frame every 3 pixels walked) but has not been compared frame by frame.
- Flame pieces: the modern core marks the last cell at full range as a tip and the rest as mid pieces. The original's exact choice when an arm is cut short has not been read from the code.
- Frames that are not 40 × 36 are cropped by the original at load time; the modern version draws the uncropped frame (same pixels, more transparent area).
