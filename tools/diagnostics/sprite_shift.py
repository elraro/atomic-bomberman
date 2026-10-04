#!/usr/bin/env python3
"""Horizontal/vertical shift of the one sprite that differs between two screenshots.

Usage: sprite_shift.py A.png B.png [y0 y1]
Both shots must show the sprite in the same pose. Prints the bounding boxes of the
sprite in A and in B (pixels that differ between the shots) and their offset.
"""
import sys
from PIL import Image, ImageChops

a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
y0, y1 = (int(sys.argv[3]), int(sys.argv[4])) if len(sys.argv) > 4 else (30, 480)
a = a.crop((0, y0, 640, y1)); b = b.crop((0, y0, 640, y1))
diff = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 24 else 0)
cols = [x for x in range(640) if diff.crop((x, 0, x + 1, diff.height)).getbbox()]
# split the differing columns into runs (old position, new position)
runs = []
for x in cols:
    if runs and x - runs[-1][1] <= 3: runs[-1][1] = x
    else: runs.append([x, x])
runs = [r for r in runs if r[1] - r[0] >= 8]
print("column runs:", runs)
if len(runs) == 2:
    print("shift left edges: %d  right edges: %d" % (runs[1][0] - runs[0][0], runs[1][1] - runs[0][1]))
