# Booting from an IC Memory Card

This project's default target is a bootable CD-ROM. `make iccard` builds
the same game for a bootable **IC Memory Card** instead - the PCMCIA-ancestor
SRAM/FRAM card that every FM TOWNS, including the Marty, has a slot for.

```sh
./run_icm.sh              # build ICMGAME.BIN and boot it in Tsugaru as a Marty
make iccard               # just build it
make iccard ICM_SIZE=8M   # a bigger card
```

A card is not a disc, and the two differences that follow from that are
the whole of this target:

* **There is no drive.** The card is memory, mapped into the physical
  address space. Nothing here reads sectors, so no CD code is compiled
  in at all - `cdrom.c` (the CDC driver), `cdda.c` (Red Book audio),
  `iso9660.c` and `scsi.c` are absent from the link. A machine booting
  off a card may have no drive at all, and a driver for a device that is
  not there can only mislead. Everything else is unchanged: the YM2612,
  the PCM DAC, the sprite and layer code, the MP2 and MBV players.
* **`const` lives on the card.** The payload's `.rodata` is *linked into
  card space*, so a const table or an embedded image is read from the
  card where it lies, in place, and costs no RAM. That is what a ROM
  cartridge gets you, and it is the reason this target is interesting
  rather than merely possible.

## How the machine gets there

The boot ROM treats the card as it treats any FM TOWNS boot device: it
looks for `IPL4` at the start of the first sector, loads that sector to
linear `0xB0000` and calls it in real mode with `BL` = 4 (IC card). Our
IPL is `src/boot/icm_ipl.S`, and it is much shorter than the CD one
(`bootsect.S` + `setup.S`), because it has nothing to read:

1. Switch to 32-bit flat protected mode - the same 4GB code/data pair at
   selectors `0x10`/`0x18` that `setup.S` installs, so `head.S` and
   everything above it cannot tell the two boot paths apart.
2. Copy the payload image out of the card into RAM at `0x10000`.
3. Jump to it.

It has to be done in that order because the card is *above* the megabyte
a real-mode segment can reach - on the 386SX at `0x00D00000`, on a 386DX
at `0xC0000000`.

`docs/HOWFMTOWNS_BOOTS_FROM_CD.txt` describes the boot ROM's IPL
handshake and the extra hoop the UX/Marty ROM adds (it reads the IPL's
`IO.SYS` extent at offset `0x20`/`0x24` before deciding to run the IPL at
all); the card IPL satisfies it exactly as the CD one does.

On real hardware, hold **I**, **C** and **M** while powering on to boot
the card. In Tsugaru that is `-BOOTKEY ICM`.

## The card's layout

`tools/mkicm.py` writes it, and patches the IPL sector with where
everything landed - nothing about the layout is a build-time constant of
the boot code:

| offset | contents |
| --- | --- |
| `0x000000` | IPL sector, 1024 bytes (all the boot ROM is known to preload) |
| `0x000400` | payload image: `.text`, `.data`, `.got` - copied to RAM at `0x10000` |
| then | payload `.rodata` - *not* copied; read where it is |
| then | asset directory (`ICMT`: name, offset, size) |
| then | the assets |

The geometry all three of the IPL, the card reader (`src/common/icm.c`)
and the packer agree on lives in one place, `src/boot/icm_defs.h`.

## Banking, and the one rule that comes with it

A 386SX - the FM TOWNS UX, and the Marty - has a 24-bit address bus, so
it sees the card through a **1MB window** at `0x00D00000`, and reaches
the rest of it by writing a 1MB bank number to I/O `0x490`. (A 386DX-class
machine sees a flat 16MB at `0xC0000000` and needs none of this; build
for it with `make iccard ICM_CARD_BASE=0xC0000000`.)

Which gives the rule that this target lives or dies by:

> **Bank 0 is the resting state of the window.** The payload's own
> `.rodata` is in bank 0. For as long as any other bank is selected, the
> program's constants are not there.

So `fmt_icm_read()` may select another bank only for the duration of a
copy - during which nothing touches a const - and puts the window back
before it returns. Getting this wrong is not a crash: with an asset
early on the card everything works, and with the same asset past the
first megabyte the MP2 decoder runs for a few seconds and then decodes
silence forever, because it is reading its own synthesis tables out of
the middle of the song. `tests/icm_read_test.c` models a real window and
a real bank register and holds the rule.

The emulator needed a fix for this too: Tsugaru's 386SX card window
ignored the bank register, so only the first megabyte of a JEIDA4 card
was ever visible (`TOWNSEMU/src/towns/memory/memaccess.cpp`,
`OldMemCardAddress`).

## Where the assets come from

The players do not know which medium they are on. `src/common/media.h`
is the vocabulary - find a file, read its first block, trickle it into a
ring buffer without blocking - and it resolves either to
`cdrom.c` + `iso9660.c` or to `icm.c`. On the card, a "read" is a copy
out of the window, so it cannot fail, cannot be late, and has no drive
state machine to wait on.

## Sizes and limits

`tools/mkicm.py` checks all of these and says so if one is exceeded:

* the IPL must fit in the first 1024-byte sector;
* the payload plus its `.rodata` plus the asset directory must fit in the
  first window (1MB on the Marty), since they are addressed without
  banking;
* `.bss` must end below `0xC0000`, where main RAM stops and the FMR VRAM
  window begins - the same check the CD build makes;
* the assets must fit in the card.

A default build - MP2 soundtrack, graphics, no video - uses about 1.4MB
of a 4MB card, of which 185KB is the payload and its read-only data.

## Inspecting a card image

`make iccard` prints the layout right after it builds `ICMGAME.BIN`, but
that is only useful while the terminal still has it. `tools/icminfo.py`
reads the same information back out of the image itself - the IPL
header and the asset directory - which also works on a card image that
did not come from this build: one downloaded, or dumped off real
hardware.

```sh
make iccard-info          # build (if needed) and inspect ICMGAME.BIN
tools/icminfo.py path/to/some.bin
```

It flags an asset whose recorded offset and size run past the end of
the file, which is what a truncated dump or a card read back with the
wrong `--size` looks like.
