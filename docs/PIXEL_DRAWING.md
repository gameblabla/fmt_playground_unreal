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
