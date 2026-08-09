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

OBJS            = src/boot/head.o src/boot/reloc.o src/boot/assets.o src/main.o \
                  src/common/common.o src/common/palette.o src/common/libfmt.o

BOOTBIN         = BOOT.BIN

all: $(BOOTBIN)

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

# Generate payload_len.h: the CD-ROM LBA the "system" payload
# (mygame_shared.bin, staged into the ISO9660 filesystem as CD/SYSTEM.BIN)
# actually starts at, and how many 2048-byte CD sectors it takes up.
#
# NOTE: PAYLOAD_LBA is NOT simply "right after the 4 preloaded CD sectors"
# (that was the old, broken assumption - see git history: a fixed LBA4
# read collides with the ISO9660 standard's mandatory 16-sector System
# Area (LBA0-15) followed immediately by the Primary Volume Descriptor at
# LBA16. Our payload (tens of KB) doesn't fit in that reserved window, so
# reading it via a fixed LBA4->LBA(4+PAYLOAD_SECTORS) span runs straight
# into the PVD/path table/directory records past LBA16 and reads garbage
# past that point instead of our payload - this was the root cause of a
# CRTC register array (and everything else past ~24KB into the payload)
# reading as corrupt/garbage at runtime despite the ELF/binary on disk
# being correct).
#
# Instead, mygame_shared.bin is staged as a normal file (CD/SYSTEM.BIN)
# in the ISO9660 filesystem, mkisofs is run once just to discover where
# it actually lands (deterministic given fixed file names/sizes in CD/,
# and independent of the -G boot image - the System Area and the
# ISO9660 Volume Descriptor Set/directory layout that follows it don't
# overlap), and that real LBA is what bootsect.S is told to read from.
CD/SYSTEM.BIN: mygame_shared.bin
	cp -f mygame_shared.bin CD/SYSTEM.BIN

src/boot/payload_len.h: CD/SYSTEM.BIN CD/IMAGE.RAW CD/PALETTE.BIN
	@rm -f /tmp/.fmtowns_lba_discover.iso; \
	mkisofs -iso-level 1 -o /tmp/.fmtowns_lba_discover.iso -pad -N -V "FROG_FEAST" \
		-D -input-charset ASCII -sysid "Win32" CD >/dev/null 2>&1; \
	lba=$$(isoinfo -i /tmp/.fmtowns_lba_discover.iso -l 2>/dev/null \
		| awk '/SYSTEM\.BIN/ { for (i=1;i<=NF;i++) if ($$i ~ /^[0-9]+$$/) last=$$i; print last }'); \
	rm -f /tmp/.fmtowns_lba_discover.iso; \
	if [ -z "$$lba" ]; then echo "Failed to discover CD/SYSTEM.BIN's LBA via isoinfo" >&2; exit 1; fi; \
	size=$$(stat -c%s mygame_shared.bin); \
	sectors=$$(( (size + 2047) / 2048 )); \
	echo "/* Auto-generated: PAYLOAD_LBA discovered from CD/SYSTEM.BIN's actual ISO9660 location */" > $@; \
	echo "#define PAYLOAD_LBA $$lba" >> $@; \
	echo "#define PAYLOAD_SECTORS $$sectors" >> $@; \
	echo "Generated $@ : PAYLOAD_LBA=$$lba PAYLOAD_SECTORS=$$sectors ($$size bytes)"

src/boot/bootsect.s: src/boot/bootsect.S src/boot/defs.h src/boot/payload_len.h
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

# ---------------------------------------------------------------------
# Final bootable image: bootsect + setup (fits in the boot ROM's 4-sector
# / 8192-byte preload window) followed immediately by the raw
# mygame_shared.bin payload, which src/boot/bootsect.S reads explicitly
# from CD-ROM starting at PAYLOAD_LBA.
# ---------------------------------------------------------------------
$(BOOTBIN): mygame_shared.bin src/boot/bootsect.o src/boot/setup.o src/boot/mygame.bin.lds
	$(LD) -T src/boot/mygame.bin.lds src/boot/bootsect.o src/boot/setup.o \
		-b binary mygame_shared.bin -o $@

clean:
	rm -f src/boot/*.o src/boot/*.s src/common/*.o src/*.o \
		src/boot/payload_len.h mygame_shared mygame_shared.bin CD/SYSTEM.BIN $(BOOTBIN)

.PHONY: all clean
