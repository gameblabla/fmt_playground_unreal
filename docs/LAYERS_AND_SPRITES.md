# FM TOWNS backgrounds and hardware sprites

`fmt_layers.h` exposes the two legal hardware arrangements:

- `fmt_set_two_backgrounds()` configures two independent RGB555 layers. Each
  layer has its own width, height, stride, CRTC display extent, and 256KB VRAM
  page. The foreground layer is selectable.
- `fmt_set_background_and_sprites()` keeps layer 0 as an RGB555 background and
  reserves layer 1 as the sprite engine's required 256x256, 512-byte-stride,
  double-buffered render target.

The two-layer aperture is linear. Layer 0 begins at `0xA00000` and layer 1 at
`0xA40000`; `0xB00000` is the single-page transformed aperture and must not be
used for layer 1. Both the video-output controller at 0x448 and the independent
layer gates at FDA0 must enable a layer.

`fmt_sprite.h` supports RGB555 patterns, positions, hiding, rotation/reflection,
half-size transforms, and enabling a suffix of the 1024-entry
priority list, busy polling, and freezing the last completed hardware page.
Pattern indices for RGB555 tiles are multiples of four.

PNG conversion
--------------

`tools/png2sprite.py` divides an RGBA PNG into 16x16 tiles, converts opaque
pixels to FM TOWNS GRB555 words, and maps transparent pixels to `0x8000`.

```sh
python3 tools/png2sprite.py hero.png build/hero.spr --header build/hero.h
```

The optional header provides original dimensions, tile dimensions, and count.
The Makefile demonstrates a grouped target so dependency tracking and parallel
builds regenerate both files safely.

Tests
-----

`make GFX_TEST=1` builds the two-background example (320x240 layer 0 plus an
independent 256x160 transparent foreground). `make GFX_TEST=2` builds the
hardware-sprite example using `assets/PISGA0.png` and `assets/SHTGB0.png`.
The default `GFX_TEST=0` remains the packed-pixel/page-flipping test.
