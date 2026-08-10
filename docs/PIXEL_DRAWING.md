# FM TOWNS packed-pixel drawing

`src/common/fmt_pixel.h` provides fixed-stride routines for the native
320x240 single-page modes:

| Routine | Transfer | Alignment |
| --- | --- | --- |
| `Set_4Pixels_320x240_8bpp` | one packed 32-bit store / four pixels | `x % 4 == 0` |
| `Set_2Pixels_320x240_8bpp` | one 16-bit store / two pixels | `x % 2 == 0` |
| `Set_Pixel_320x240_8bpp` | masked byte store / one pixel | none |
| `Set_2Pixels_320x240_15bpp` | one packed 32-bit store / two RGB555 pixels | `x % 2 == 0` |
| `Set_Pixel_320x240_15bpp` | one 16-bit store / one RGB555 pixel | none |

The 8bpp packed value is little-endian: the first on-screen pixel is bits
7..0.  The 15bpp pair is likewise `color0 | color1 << 16`.  Color values are
RGB555 words; use `rgb15()` from `common.h`.

Why the address transform is needed
-----------------------------------

Single-page VRAM is banked rather than row-major in the CPU-visible native
window.  The transform matches TOWNSEMU's
`TownsSinglePageVRAMAddressTransform::SinglePageOffsetToLinearOffset()`:

```c
((offset & 4) << 16) | ((offset & 0x7fff8) >> 1) | (offset & 3)
```

This places each aligned four-byte packed transfer on the correct four
on-screen pixels.  It also exposes the useful hardware grouping: an aligned
32-bit store drives both 16-bit VRAM banks, while a 16-bit store uses one.

The single 8bpp routine selects one byte with packed-pixel mask registers 0
and 1 through ports 0x458/0x45A/0x45B, performs a byte transfer,
and restores the mask to 0xffffffff.  Restoring it is important because the
mask is global hardware state and otherwise changes later VRAM writes.

Verification pattern
--------------------

The example does not load an image.  It sets each mode, clears all 320x240
pixels to black with the widest routine, waits for a vertical sync, and draws
known-size geometry:

- 8bpp: 304x48 red, green, and blue bars made with the 4-, 2-, and 1-pixel
  routines respectively, plus a one-pixel white border.
- 15bpp: a 304x72 alternating red/green bar made with paired stores, a
  304x80 blue bar made with single stores, and a one-pixel white border.

The Makefile defaults to the 8bpp test. Build the other screen with
`make clean && make PIXEL_TEST_BPP=15`. Keeping each test on screen avoids a
timing-dependent capture. Native 320x240 screenshots therefore make off-by-one,
missing-bank, half-width, and overdraw errors measurable rather than subjective.

Hardware page flipping
----------------------

The 256x240 8bpp, 320x240 8bpp, and 320x240 RGB555 modes fit two complete
frame buffers in the FM TOWNS' 512KB VRAM. `fmt_set_mode()` clears both,
displays buffer 0, and selects buffer 1 for drawing. Every packed-pixel helper
adds `g_fmt_draw_buffer_offset` before applying the single-page bank transform,
so drawing cannot modify the displayed buffer.

Call `fmt_flip_page()` only after the frame is complete. It waits for a fresh
vertical blank, writes the hidden buffer's start to CRTC `FA0`, then selects
the old displayed buffer for the next frame. `FA0` is not a byte address: in
these modes one count is eight bytes, so the programmed value is
`page_byte_offset / 8`. `FA1` belongs to layer 1 and must not be changed for
single-layer page flipping.

The 640x400, 512x480, and 640x480 RGB555 modes cannot use this API: one frame
uses 512000, 491520, and 614400 bytes respectively, leaving insufficient VRAM
for a second frame (and the last mode already exceeds native 512KB VRAM).
`fmt_page_flipping_available()` is false and `fmt_flip_page()` returns -1 for
those modes.
