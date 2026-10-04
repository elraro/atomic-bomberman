#!/usr/bin/env python3
"""Extract frames of an Atomic Bomberman .ANI file to PNG and list its sequences.

Usage: ani_extract.py FILE.ani OUT_DIR [--sheet]
Output goes to OUT_DIR, which must be outside the original game directory.
"""
import os
import sys

from PIL import Image

import ani


def to_image(frame):
    img = Image.new("RGBA", (max(frame.width, 1), max(frame.height, 1)))
    px = []
    for v in frame.pixels:
        if (frame.flags & 4) and v == frame.key:
            px.append((0, 0, 0, 0))
        else:
            r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
            px.append((r * 255 // 31, g * 255 // 31, b * 255 // 31, 255))
    img.putdata(px)
    return img


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    chunks = ani.load(path)
    frs = ani.frames(chunks)
    images = []
    for i, f in enumerate(frs):
        if f is None:
            continue
        img = to_image(f)
        images.append(img)
        if "--sheet" not in sys.argv:
            img.save(os.path.join(out, f"frame{i:03d}.png"))
        print(f"frame {i:3d} {f.name:14s} {f.width}x{f.height} hot=({f.hot_x},{f.hot_y}) flags={f.flags} key={f.key}")
    if "--sheet" in sys.argv and images:
        cw, ch = max(i.width for i in images), max(i.height for i in images)
        cols = min(len(images), 12)
        rows = (len(images) + cols - 1) // cols
        sheet = Image.new("RGBA", (cols * cw, rows * ch), (40, 40, 60, 255))
        for n, img in enumerate(images):
            sheet.paste(img, ((n % cols) * cw, (n // cols) * ch), img)
        sheet.save(os.path.join(out, "sheet.png"))
    for name, states in ani.sequences(chunks):
        print(f"seq '{name}': frames {[s[0] for s in states]}")


if __name__ == "__main__":
    main()
