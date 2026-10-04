# asset-extractor

New code. Reads original Atomic Bomberman data files supplied by the user; contains no original material.

| File | Purpose |
|---|---|
| `ani.py` | Reader for `.ani` files: chunk tree, frames (16-bit, RLE), sequences |
| `ani_extract.py` | `ani_extract.py FILE.ani OUT_DIR [--sheet]` writes PNG frames (or one sheet) and prints frame and sequence tables |

Requires Python 3 and Pillow. Write output outside `game/`; extracted images are original artwork and must not be committed or redistributed.

Status (2026-10-04): 94 of the 95 `.ani` files decode (2299 frames, 235 sequences). `classics.ani` uses an image type (11) and encoding (0x10) that the game itself rejects; it is not supported.
