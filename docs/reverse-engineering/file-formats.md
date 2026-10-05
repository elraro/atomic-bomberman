# File Formats

Formats are identified from content, not extension. Text formats are summarised in `initial-analysis.md` §8 and `maps.md`; this file holds the binary formats.

## .ANI — sprite frames and animation sequences

Confidence: HIGH for everything marked ✔ (implemented in `tools/asset-extractor/ani.py` and checked on 94 files, 2299 frames; bomb and walk frames inspected visually). Loader in the original: `ani.c`, `0x41C837` (frames), `0x41CD03` (sequences), `0x41C0BA` (RLE).

All integers little-endian.

### Container ✔

```text
offset 0   char[10]  "CHFILEANI "
offset 10  u32       length of everything after this 16-byte header
offset 14  u16       0
offset 16  chunks…
```

Chunk:

```text
char[4]  tag
u32      body length
u16      id
body
```

Top-level chunks, in file order:

| Tag | Body | Meaning |
|---|---|---|
| `HEAD` | 48 bytes | File header. 4 variants seen across the 95 files; fields not decoded |
| `PAL ` | 8192 bytes | Not decoded. Not needed to decode frames |
| `TPAL` | 1028 bytes | Not decoded |
| `CBOX` | 4 bytes | Two u16, e.g. 16, 16. Not decoded |
| `FRAM` | container | One per frame |
| `SEQ ` | container | One per named sequence |

### FRAM ✔

Sub-chunks: `HEAD` (2 bytes, not decoded), `FNAM` (source image name, NUL-terminated, e.g. `BMB1001.TGA`), `CIMG` (image).

`CIMG` body:

```text
0   u16  type            low 3 bits == 4 → 16 bits per pixel (the only type the game accepts)
2   u16  flags           bit 2: `key` is the transparent colour
4   u32  data offset     0x18
8   u32  (second offset, 0 in all files)
12  u16  width
14  u16  height
16  u16  hot-spot x
18  u16  hot-spot y
20  u32  key colour      0 or 0x7FFF in the shipped files
--- at data offset ---
+0  u8   encoding        0x00 raw, 0x11 run-length
+1  u8   0
+2  u16  length of this sub-header (0x0C)
+4  u32  stored size, including this 12-byte sub-header
+8  u32  uncompressed size = width × height × 2
+12 data
```

Pixels are 16-bit RGB555 (`0RRRRRGGGGGBBBBB`), row-major, top to bottom.

Encoding 0x11 ✔: read a control byte `c`. `0xFF` ends the stream. If `c & 0x80`: the next 16-bit value repeats `(c & 0x7F) + 1` times. Otherwise `c + 1` literal 16-bit values follow.

What the game does after decoding: each RGB555 value is converted to an 8-bit palette index through a 32 768-entry table at `0x495390`; key-coloured pixels become index 0; frames that are not exactly 40 × 36 are cropped to their non-zero bounding box and the hot-spot is adjusted.

The hot-spot is the object's reference point. For full-cell frames it is (20, 35): horizontal centre, bottom row, which matches `Map_CellToPixelX/Y`.

### SEQ ✔ (structure), partly decoded (fields)

Sub-chunks: one `HEAD` (96 bytes) then one `STAT` per animation step.

- `SEQ /HEAD`: sequence name, NUL-terminated, at offset 0 (e.g. `walk north`, `bomb regular green`, `flame tipsouth green`). The number of states is stored near offset 0x50. Other bytes look like uninitialised memory from the authoring tool.
- `STAT`: `HEAD` (46 bytes: first u16 is 0x001E or 0xFFFF, rest zero; meaning unknown) and `FRAM` (12 bytes, or 22 in three cases):

```text
0  u16  1            (count of frame references; MEDIUM)
2  u16  frame index  index into the file's FRAM chunks ✔
4  i16  x offset     (MEDIUM: small values such as −1, 0, 1)
6  i16  y offset     (MEDIUM)
8  4 bytes zero
```

The game looks sequences up by name (`Ani_FindSequence 0x41D957`) and addresses a step with `Ani_GetSequenceFrame` (`0x41DAA7`), wrapping by `Ani_GetSequenceLength` (`0x41DA5C`).

Names ending in `green` are drawn through a colour remap so that the green parts take the player's colour.

### Not supported

`classics.ani` contains 28 frames of CIMG type 11 with encoding 0x10. The game's loader rejects both (`unsupported bits per unit count`, `unsupported enctype`), so the file is probably unused. Not decoded.

## color.pal

33 536 bytes = 768 + 32 768.

- First 768 bytes: 256 RGB triples. Values are 6-bit (0-63) except entry 0 (`FF FF FF`). Confidence: MEDIUM-HIGH.
- Remaining 32 768 bytes: one byte per RGB555 colour, giving the nearest palette index. This is consistent with the 32 768-entry table the ANI loader indexes with RGB555 pixels (`0x495390`). That the table in memory is loaded from this part of the file has not been traced. Confidence: MEDIUM.

## fontN.fon — bitmap fonts

Confidence: HIGH (the computed size equals the file size for all three shipped fonts; text rendered with `font1.fon` reproduces the original's HUD labels).

```text
0   i32  glyph count           (128 for font0 and font6, 256 for font1)
4   i32  glyph height in pixels (17, 16, 16)
8   i32  extra spacing between glyphs (1, 0, 0)
12  8 bytes of run-time pointers (ignored)
20  per glyph: i32 width, i32 offset into the bitmap area
... bitmap area: for each glyph, `height` rows of ceil(width / 8) bytes, most significant bit = leftmost pixel
```

The game looks for `font0.fon` … `font9.fon`; only 0, 1 and 6 exist. The HUD score labels use `font1.fon`.

The round clock is **not** drawn with these fonts: its digits are sprites, sequence `numeric font` in `kfont.ani` (frames 0-9 and a colon at index 10), and the "hurry" banner is the single frame of `hurry.ani`.

## N.rmp (0-9)

259 bytes: 256-byte palette index remap followed by three bytes that equal the RGB percentages listed for that player colour in `valuelst.res` 200-247. How and when the remap is applied to "green" sprites is not traced (UNKNOWN-021).

## .rss

Headerless PCM, 22 050 Hz, stereo, 16-bit signed little-endian, according to the header comment of `soundlst.res`. Not yet verified by playback.

## .pcx

Standard ZSoft PCX version 5, 8 bits per pixel, 640 × 480 for backgrounds.

## Intro movie (`intro/bmintro.exe`, Interplay MVE)

Confidence: HIGH for the container and the pictures (all 839 decode to clean images); MEDIUM for the sound (decoded without clipping or drift, but not listened to).

`bmintro.exe` is a small player program with the movie appended: the signature `Interplay MVE File\x1A\0` followed by the words 0x001A, 0x0100, 0x1133 is at file offset 70656 (an earlier occurrence of the text at 53468 is a string of the program). The movie runs to the end of the file.

| Property | Value |
|---|---|
| Pictures | 839, 480 × 280 (60 × 35 blocks of 8 × 8), 8-bit with a palette of 6-bit components |
| Timing | 8341 µs × 8 = 66.7 ms per picture (15 per second), 56.0 s |
| Sound | 22 050 Hz, stereo, 16-bit, Interplay DPCM (one step-table byte per sample; each chunk starts with one raw sample per channel) |

Container: chunks of `u16 length, u16 type`; inside, operations of `u16 length, u8 type, u8 version`. Used here: 0x02 timer, 0x03 sound format, 0x05 picture size, 0x0C palette, 0x0F block-code map (4 bits per block), 0x11 picture data (14-byte header), 0x08 sound data (sequence, stream mask, decoded length), 0x07 show the picture, 0x00 end. The sixteen block codes copy a block from the previous or the one-before-previous picture (with or without a displacement), from the picture being built, or paint it with 1, 2, 4 or 64 colours at several resolutions.

The game program (`bm95.exe`) does not play this movie; the original starts the player separately. Modern: `src/resources/mve_file.*` (decoder, tested with a hand-built movie), `tools/diagnostics/mve_dump.cpp`; the importer copies only the movie part to `intro.mve`; the game plays it before the logo screens and any of Enter, Space or Esc skips it.
