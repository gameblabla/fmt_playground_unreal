#!/usr/bin/env python3
"""Convert RGBA PNGs to tiled FM TOWNS 16x16 RGB555 sprite patterns."""
import argparse
from pathlib import Path
from PIL import Image

p=argparse.ArgumentParser()
p.add_argument("input")
p.add_argument("output")
p.add_argument("--header")
a=p.parse_args()
im=Image.open(a.input).convert("RGBA")
tx=(im.width+15)//16; ty=(im.height+15)//16
out=bytearray()
for cy in range(ty):
 for cx in range(tx):
  for y in range(16):
   for x in range(16):
    px=cx*16+x; py=cy*16+y
    r,g,b,al=im.getpixel((px,py)) if px<im.width and py<im.height else (0,0,0,0)
    v=0x8000 if al<128 else ((g>>3)<<10)|((r>>3)<<5)|(b>>3)
    out += bytes((v&255,v>>8))
Path(a.output).write_bytes(out)
if a.header:
 n=Path(a.output).stem.upper().replace('-','_')
 Path(a.header).write_text(f"#define {n}_WIDTH {im.width}\n#define {n}_HEIGHT {im.height}\n#define {n}_TILES_X {tx}\n#define {n}_TILES_Y {ty}\n#define {n}_TILE_COUNT {tx*ty}\n")
