Example FM TOWNS MARTY CD program
===============================

See https://github.com/pinterior/elf2exp/
to set up toolchain.

No IO.SYS, no TOWNS OS and no DOS extender are needed: the disc boots
through its own "IPL4" boot sector straight into 32-bit protected mode.

Use RUN.SH to build the disc image and run it with Tsugaru_CUI.elf.

How it boots
------------

`make` builds two things:

* `IPL.BIN` - the boot sector (`src/boot/bootsect.S` + `src/boot/setup.S`).
  The FM TOWNS boot ROM looks for the magic string `IPL4` at the very
  start of the disc, preloads the first 4 sectors (8192 bytes) to
  0xB0000 and calls into them, so this is all it can be.
* `CD/SYSTEM.BIN` - the 32-bit payload, staged as an ordinary file in
  the ISO9660 filesystem and read back by the IPL over the boot ROM's
  disk BIOS.

`tools/mkcd.sh` then builds the filesystem and calls
`tools/inject_ipl.py`, which writes `IPL.BIN` into the image's reserved
System Area and patches into it where `SYSTEM.BIN` actually landed. That
ordering matters: the payload's LBA is a property of the finished image,
so it is read out of the image itself rather than guessed beforehand.
`tools/inject_ipl.py` is a standalone script and works on any ISO -
it is the modern equivalent of Fujitsu's DOS-era `IPL.COM`.

`tools/iso_sort.txt` pins `SYSTEM.BIN` to the front of the disc, because
the boot ROM's disk read only takes a 16-bit sector number.

See `docs/HOWFMTOWNS_BOOTS_FROM_CD.txt` for the boot ROM's side of this,
including why the IPL still has to claim to have an IO.SYS (bytes 0x20
and 0x24) for the UX and Marty ROMs to hand it control.

This example loads image from CD, loads it into buffer, draws it to VRAM with double buffering/paging and wait for vsync.