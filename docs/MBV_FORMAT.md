# MBV - Marty Block Video

MBV is the codec sketched in `video.txt`, implemented: a block video format
whose decoder is nothing but `memcpy`, `memset`, small table lookups and mask
expansion, because the machine it has to run on is a 16MHz 386SX that is
simultaneously feeding a DAC every 62.5us and pulling sectors off a CD by PIO.

At a glance:

| | |
|---|---|
| Picture | 256x240, 8bpp indexed (`FMT_MODE_256x240_8BPP`, shown as 512x480 through 2x CRTC zoom) |
| Sound | 16kHz unsigned 8-bit mono, interleaved per frame, out of the YM2612 channel-6 DAC |
| Frame rate | anything; 12fps is what the tooling defaults to |
| Typical rate | 60-70 KB/s video at 12fps for anime source, against a 1x CD's ~150 KB/s |
| Files | `src/common/mbv.[ch]` decoder, `src/common/mbvplay.[ch]` player, `tools/mbvenc.c` encoder |

Everything is little-endian and byte-aligned. There is no bit reader anywhere
in the decoder: the entire video payload is walked with one forward pointer.

## Why 8bpp indexed and not 16bpp

The mode list has a 256x240 16bpp variant, and it is the wrong choice here. A
palette index *is* the machine's pixel in 8bpp, so a block type can carry
"these 16 pixels use these two colours" in four bytes and the decoder writes
them straight to the frame buffer with no conversion step. In 16bpp the same
block costs twice the bytes and twice the VRAM traffic, on a machine that has
neither to spare - and 8bpp halves the blit, which is the second most expensive
thing the player does. This is `video.txt`'s argument for eliminating the YUV
abstraction, taken one step further.

## File layout

```
Header             32 bytes
Frame chunk 0      8-byte header + palette + audio + video
Frame chunk 1
...
```

### Header

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | `"MBV1"` |
| 4 | 2 | width in pixels, multiple of 8 |
| 6 | 2 | height in pixels, multiple of 8 |
| 8 | 2 | frame rate, 8.8 fixed point (12fps = 3072) |
| 10 | 1 | bits per pixel, always 8 |
| 11 | 1 | flags; bit 0 = audio present |
| 12 | 4 | frame count |
| 16 | 4 | audio sample rate in Hz |
| 20 | 4 | largest chunk in the file, its header included |
| 24 | 4 | total audio samples |
| 28 | 4 | reserved, zero |

`max_chunk` exists so the player can refuse a file it cannot buffer before it
starts playing it, rather than discovering the problem two seconds in.

### Frame chunk

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | payload bytes following this header |
| 4 | 2 | audio bytes in this chunk |
| 6 | 1 | frame type: 0 = inter, 1 = key |
| 7 | 1 | palette patch count (inter frames only) |

followed by, in order:

* **palette** - a keyframe carries all 256 entries as RGB888 triplets (768
  bytes). An inter frame may instead carry `pal_patch` records of 4 bytes each:
  index, R, G, B. (The decoder implements patches; the encoder currently only
  emits full palettes at keyframes.)
* **audio** - `audio_bytes` unsigned 8-bit mono samples at the header's rate.
  These are DAC-ready: `0x80` is silence and the player writes them to the
  YM2612 data register unmodified.
* **video** - the block opcode stream, filling the rest of the chunk.

Each chunk carries exactly the audio belonging to its own frame. That is what
keeps the picture in step with the sound: the player treats the audio as its
clock and shows a frame when the previous frame's samples have played out, so
a frame that takes too long to decode stretches both, and they cannot drift
apart. See `src/common/mbvplay.h`.

## Video: the block stream

The frame is a grid of 8x8 macroblocks in raster order. Motion works on 8x8,
texture on 4x4 - `video.txt`'s compromise, kept.

### Macroblock opcodes

| Byte | Operands | Meaning |
|---|---|---|
| `0x00`-`0x3F` | - | skip the next *n*+1 macroblocks, untouched |
| `0x40` | 1 vector byte | copy 8x8 from elsewhere in the frame |
| `0x41` | 1 index | fill 8x8 with one colour |
| `0x42` | four 4x4 blocks | split |
| `0x43` | 64 indices | raw 8x8, row-major |

### 4x4 block opcodes (inside `0x42` only)

| Byte | Operands | Bytes | Meaning |
|---|---|---|---|
| 0 | - | 1 | skip: unchanged from the previous frame |
| 1 | 1 index | 2 | fill |
| 2 | 2 indices + 2 map bytes | 5 | 2 colours, 1 bit per pixel |
| 3 | 4 indices + 4 map bytes | 9 | 4 colours, 2 bits per pixel |
| 4 | 8 indices + 6 map bytes | 15 | 8 colours, 3 bits per pixel |
| 5 | 16 indices | 17 | raw |
| 6 | 1 vector byte | 2 | copy 4x4 from elsewhere in the frame |

Pixel maps are LSB-first in row-major order. The 1bpp map is a 16-bit
little-endian word; the 2bpp map is one byte per row; the 3bpp map does not
divide a four-pixel row, so it is carried as two 24-bit little-endian groups of
eight pixels - two rows each.

### Motion vectors

One byte: high nibble dx, low nibble dy, each biased by 8, so both cover -8 to
+7. The source rectangle must lie entirely inside the frame; the decoder
rejects a chunk where it does not.

## In-place reconstruction

There is exactly one frame buffer. SKIP costs nothing because the pixels are
simply not touched, which is the property the whole format is built around -
and it is only free if there is no second buffer to copy from.

The consequence is that MOTION reads the buffer it is writing, so a vector
pointing up or left can source pixels that this very frame has already changed.
That is part of the format's definition, not a bug to be designed around: the
copy is byte-by-byte in row-major order, and the encoder reconstructs by
calling the decoder's own primitives (`fmt_mbv_*` in `src/common/mbv.c`) in
stream order, so it measures the actual result of the actual copy. Encoder and
decoder cannot disagree, and no error can accumulate across a GOP.

`tests/mbv_roundtrip_test.c` checks that claim on real encoded output, frame by
frame and byte for byte, palette included. It is part of `make test`.

## Palettes

Each GOP gets its own 256-colour palette, built by median cut over that GOP's
frames, and it changes only at keyframes - where every macroblock is coded
anyway, so no block can reference indices from the old palette. Keeping the
palette still between keyframes is what avoids the flicker that per-frame
quantisation produces, and it means most frames spend nothing on colour at all.

### Getting a new palette in without a visible glitch

The palette DAC is consulted per pixel as the CRTC scans out, so an entry
written after the blanking interval recolours the rest of that frame from the
current raster line down. There is not much interval: in this mode `VDS0` puts
the first displayed line 70 half-lines into a 1050 half-line frame, i.e.
**1.11ms** after the vertical sync edge `fmt_flip_page()` returns on. (The sync
pulse itself is only 64us, so waiting for it to end would leave nothing.)
Writing all 256 entries one at a time with a DAC tick between each came to
about a millisecond - right at the edge, and over it often enough to show as a
brief wrong-colour flash at a GOP boundary.

Both ends of the pipeline now work to keep the upload small:

* **The encoder renumbers each new palette** so every entry sits in the slot
  held by the nearest colour of the previous palette, and replaces entries that
  land within `-p` (default 108, squared RGB distance - about 6 levels per
  channel) of what is already there with that existing colour exactly. The
  permutation is free, because indices are assigned before the GOP is encoded.
  Cost is under 0.05 dB.
* **The player keeps a shadow of what the DAC holds** and writes only the
  entries that differ, works out *which* ones before the flip rather than
  inside the window, and ticks the DAC every eight entries instead of every
  one.

On `sailor.mkv` that takes an ordinary keyframe from 256 entries and ~1ms to
45-101 entries and **115-258us**. The one genuine scene cut still replaces all
256, in **653us** - still inside the 1.11ms, with 40% to spare. Those figures
are measured in-machine against the free-running 1us counter and published in
`MBV_STATS[6..9]`; a scene cut is written out in full rather than deferred to
the next blanking interval, because running slightly long costs one frame with
a seam whereas deferring would leave the whole picture miscoloured for 83ms.

## Encoding

```
tools/mkmbv.sh sailor.mkv CD/VIDEO.MBV -f 12 -g 24 -b 5000
```

`mkmbv.sh` has ffmpeg scale to 256x240 and resample to 16kHz mono u8, then runs
`build/mbvenc` on the raw streams. The encoder chooses blocks by
rate-distortion - each candidate coding costs `error + lambda * bytes`, cheapest
wins - with lambda steered by a feedback loop towards a byte budget per frame,
because the binding constraint is bandwidth and CPU rather than quality. Useful
knobs:

| Flag | Default | Meaning |
|---|---|---|
| `-f` | 12 | frame rate |
| `-g` | 24 | keyframe/palette interval |
| `-b` | 5000 | byte budget per inter frame |
| `-k` | 20000 | byte budget per keyframe |
| `-m` | 30000 | hard chunk ceiling; must fit `MBV_SCRATCH` in `mbvplay.c` |
| `-s` | 8 | motion search radius in pixels |
| `-p` | 108 | palette snap threshold, squared RGB distance; 0 disables |

## Playing it

```
make VIDEO_PLAYER=1        # builds the payload and encodes CD/VIDEO.MBV
./tools/mkcd.sh            # data-only ISO; the soundtrack is inside the video
./Tsugaru_CUI.elf "$PWD/MARTY_ROM/" -TOWNSTYPE MARTY -CD output.iso -NORMALFD -DONTUSEFPU
```

`make VIDEO_PLAYER=1 MBV_STATS=1` additionally publishes progress counters at
physical `0x00100000`, readable while it runs with
`MEMDUMP PHYS:00100000 44 1` in the emulator console: stage, frames presented,
bytes consumed, DAC underruns, the streaming reader's state, the measured
vertical sync pulse, and the size and duration of the last palette upload.
Underruns should be zero.

## Where the time goes

Per frame at 12fps there are ~83ms, and the four jobs are:

* **DAC**, one sample every 62.5us. The hard deadline, met by polling
  `fmt_dac_tick()` from inside everything else - nothing in this payload is
  interrupt-driven. The frame decoder ticks once per macroblock and the blit
  every 64 bytes, both well inside a sample period.
* **decode**, which for a typical inter frame touches only the blocks that
  changed.
* **blit**, 61440 bytes into VRAM. `src/common/mbv_blit.h` exploits the fact
  that libfmt's single-page VRAM swizzle turns the source's even and odd
  four-byte groups into two dense linear arrays 256KB apart, so the whole frame
  is one loop of 32-bit writes down two pointers instead of 61440 byte writes
  each with an address computation. `tests/mbv_blit_test.c` checks the
  derivation against libfmt's own transform for every byte of both pages.
* **the drive**, stepped one I/O operation at a time from every wait in the
  player's loop.

## A note on RAM

The payload loads at 0x10000 and everything in it - text, data and `.bss` -
has to end below 0xC0000, where FM TOWNS main RAM stops and the FMR VRAM
window begins (RAM resumes at 0x100000). This player owns ~160KB of buffers,
so the Makefile links in only the one large player selected by the build flags
and checks the payload's end address after linking. The check exists because
the failure is silent: `.bss` past the line is not memory, writes to it land in
the VRAM window, and the first symptom is a CD ring buffer full of 0xFF that
the drive never put there.
