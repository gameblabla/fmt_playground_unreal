# FM TOWNS (Marty) bare-metal CD boot example.
#
# Boots directly via a custom FM TOWNS "IPL4" CD-ROM boot sector
# (src/boot/bootsect.S), through a real-mode -> 32-bit flat protected-mode
# transition (src/boot/setup.S, src/boot/head.S), with no DOS, no RUN386
# DOS extender, and no ISO9660 filesystem driver at runtime. Structure
# mirrors fmtowns_playground's floppy-boot layout, adapted for CD-ROM LBA
# reads and a fixed, known payload load address (so no PIC self-relocation
# quirks are introduced beyond what fmtowns_playground already relies on).

AS              = as -32
CC              = gcc
LD              = ld -s

CFLAGS          = -Isrc/boot -Isrc/common -Isrc -Wall -march=i486 -std=gnu99 -m32 -Os \
                  -fomit-frame-pointer -fno-builtin -ffreestanding \
                  -fPIC -Wno-stack-protector -fno-stack-protector -s

PIXEL_TEST_BPP  ?= 8
CFLAGS          += -DFMT_PIXEL_TEST_BPP=$(PIXEL_TEST_BPP)

OBJS            = src/boot/head.o src/boot/reloc.o src/boot/assets.o src/main.o \
                  src/common/common.o src/common/palette.o src/common/libfmt.o src/common/pad.o \
                  src/common/cdrom.o src/common/sound.o src/common/iso9660.o src/common/pcmstream.o \
                  src/common/cdda.o

IPLBIN          = IPL.BIN

all: $(IPLBIN) CD/SYSTEM.BIN

# ---------------------------------------------------------------------
# 32-bit "system" payload (head.S + reloc.c self-relocation + main.c),
# built exactly like fmtowns_playground's mygame_shared: once statically
# (to capture the GOT/relocation layout) then again as a PIC shared
# object (-Bsymbolic), which is what actually gets objcopy'd to a flat
# binary and embedded in the boot image.
# ---------------------------------------------------------------------
mygame_shared: $(OBJS) src/boot/mygame_shared.lds Makefile
	$(LD) --warn-constructors --warn-common -static -T src/boot/mygame_shared.lds \
		-o $@ $(OBJS) && \
	$(LD) -shared -Bsymbolic -T src/boot/mygame_shared.lds -o $@ $(OBJS)

mygame_shared.bin: mygame_shared
	objcopy -O binary $< mygame_shared.bin

# Stage the "system" payload into the ISO9660 filesystem as an ordinary
# file. bootsect.S reads it back by raw LBA, but it does NOT know that
# LBA at build time: tools/inject_ipl.py patches the real one into the
# IPL after the image has been built (see tools/mkcd.sh).
#
# The payload deliberately isn't part of the boot image itself. The boot
# ROM preloads only the first 4 CD sectors (8192 bytes) and runs them,
# and the 16 sectors it would have to live in past that are the ISO9660
# System Area, immediately followed by the Primary Volume Descriptor at
# LBA 16 - nowhere near enough room for a payload this size (an earlier
# version read it from a fixed LBA 4 and got the PVD, path table and
# directory records instead, garbage from ~24KB in).
CD/SYSTEM.BIN: mygame_shared.bin
	cp -f mygame_shared.bin CD/SYSTEM.BIN

src/boot/bootsect.s: src/boot/bootsect.S src/boot/defs.h
	$(CC) -E -traditional -Isrc/boot $< -o $@

src/boot/setup.s: src/boot/setup.S src/boot/defs.h
	$(CC) -E -traditional -Isrc/boot $< -o $@

src/boot/head.s: src/boot/head.S src/boot/defs.h src/boot/config.h src/boot/test.h
	$(CC) -E -traditional -Isrc/boot $< -o $@

src/boot/bootsect.o: src/boot/bootsect.s
	$(AS) -o $@ $<

src/boot/setup.o: src/boot/setup.s
	$(AS) -o $@ $<

src/boot/head.o: src/boot/head.s
	$(AS) $(ASFLAGS) -o $@ $<

src/boot/assets.o: src/boot/assets.S CD/IMAGE.RAW CD/PALETTE.BIN
	$(AS) -I. -o $@ $<

src/boot/reloc.o: src/boot/reloc.c
	$(CC) -c $(CFLAGS) -fno-strict-aliasing -o $@ src/boot/reloc.c

src/main.o: src/main.c
	$(CC) -c $(CFLAGS) -o $@ src/main.c

src/common/common.o: src/common/common.c
	$(CC) -c $(CFLAGS) -o $@ src/common/common.c

src/common/palette.o: src/common/palette.c
	$(CC) -c $(CFLAGS) -o $@ src/common/palette.c

src/common/libfmt.o: src/common/libfmt.c src/common/libfmt.h
	$(CC) -c $(CFLAGS) -o $@ src/common/libfmt.c

src/common/pad.o: src/common/pad.c src/common/pad.h
	$(CC) -c $(CFLAGS) -o $@ src/common/pad.c

src/common/scsi.o: src/common/scsi.c src/common/scsi.h
	$(CC) -c $(CFLAGS) -o $@ src/common/scsi.c

src/common/cdrom.o: src/common/cdrom.c src/common/cdrom.h
	$(CC) -c $(CFLAGS) -o $@ src/common/cdrom.c

src/common/sound.o: src/common/sound.c src/common/sound.h
	$(CC) -c $(CFLAGS) -o $@ src/common/sound.c

src/common/iso9660.o: src/common/iso9660.c src/common/iso9660.h src/common/cdrom.h
	$(CC) -c $(CFLAGS) -o $@ src/common/iso9660.c

src/common/pcmstream.o: src/common/pcmstream.c src/common/pcmstream.h src/common/cdrom.h src/common/iso9660.h
	$(CC) -c $(CFLAGS) -o $@ src/common/pcmstream.c

src/common/cdda.o: src/common/cdda.c src/common/cdda.h src/common/cdrom.h
	$(CC) -c $(CFLAGS) -o $@ src/common/cdda.c

# ---------------------------------------------------------------------
# The IPL: bootsect + setup, which is all the boot ROM ever preloads
# (4 CD sectors / 8192 bytes) and runs. tools/inject_ipl.py writes this
# into the ISO9660 System Area of the finished image and enforces the
# 8192-byte ceiling. The payload rides along separately as CD/SYSTEM.BIN.
# ---------------------------------------------------------------------
$(IPLBIN): src/boot/bootsect.o src/boot/setup.o src/boot/ipl.bin.lds
	$(LD) -T src/boot/ipl.bin.lds src/boot/bootsect.o src/boot/setup.o -o $@

clean:
	rm -f src/boot/*.o src/boot/*.s src/common/*.o src/*.o \
		mygame_shared mygame_shared.bin CD/SYSTEM.BIN $(IPLBIN) BOOT.BIN \
		src/common/pad.o src/common/scsi.o src/common/cdrom.o src/common/sound.o \
		src/common/iso9660.o src/common/pcmstream.o src/common/cdda.o

.PHONY: all clean
