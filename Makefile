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

CFLAGS          = -Ibuild -Isrc/boot -Isrc/common -Isrc -Wall -march=i386 -std=gnu99 -m32 -Os \
                  -fomit-frame-pointer -fno-builtin -ffreestanding \
                  -fPIC -Wno-stack-protector -fno-stack-protector -s

PIXEL_TEST_BPP  ?= 8
GFX_TEST        ?= 0
VGM_PLAYER      ?= 0
MP2_PLAYER      ?= 1
VIDEO_PLAYER    ?= 0
TEST_256_MODE   ?= 0
BUSY_PROBE      ?= 0
MBV_STATS       ?= 0
CFLAGS          += -DFMT_PIXEL_TEST_BPP=$(PIXEL_TEST_BPP) -DFMT_GFX_TEST=$(GFX_TEST) \
                  -DFMT_VGM_PLAYER=$(VGM_PLAYER) -DFMT_MP2_PLAYER=$(MP2_PLAYER) \
                  -DFMT_TEST_256x240=$(TEST_256_MODE) -DFMT_YM_BUSY_PROBE=$(BUSY_PROBE) \
                  -DFMT_VIDEO_PLAYER=$(VIDEO_PLAYER)

VGM_SOURCE      = artifacts/neo_holy_war/neo_holy_war_trimmed_opn2_only_optimized.vgm
VGM_ASSET       = CD/MUSIC.FTV
MP2_SOURCE      = Shattered_Decks_French.wav
MP2_ASSET       = CD/MUSIC.MP2
MBV_SOURCE      = sailor.mkv
MBV_ASSET       = CD/VIDEO.MBV

OBJS            = src/boot/head.o src/boot/reloc.o src/boot/assets.o src/main.o \
                  src/common/common.o src/common/palette.o src/common/libfmt.o src/common/pad.o \
                  src/common/fmt_layers.o src/common/fmt_sprite.o \
                  src/common/cdrom.o src/common/sound.o src/common/iso9660.o src/common/pcmstream.o src/common/dacout.o src/common/mp2.o src/common/mp2_fast.o src/common/kjmp2_fast.o src/common/mp2stream.o \
                  src/common/cdda.o

# The two large players are linked in only when they are the one selected.
# Each owns hundreds of KB of .bss - vgmplay.c a 384KB command buffer,
# mbvplay.c ~160KB of frame, ring and chunk buffers - and the payload has to
# fit under PAYLOAD_LIMIT below, so linking both in at once does not fit and
# would not be used anyway (src/main.c picks exactly one player).
ifneq ($(VGM_PLAYER),0)
OBJS            += src/common/vgmplay.o
endif
ifneq ($(VIDEO_PLAYER),0)
OBJS            += src/common/mbv.o src/common/mbvplay.o
endif

IPLBIN          = IPL.BIN

# Which player and which tests get compiled in is chosen entirely by the
# variables above, and make cannot see a variable change - so an object built
# for one configuration is happily reused for the next, and the payload ends
# up half one build and half another.  This bit me: `make VIDEO_PLAYER=1`
# after a plain `make` relinked the video player's objects but kept the
# src/main.o that had been compiled with FMT_VIDEO_PLAYER=0, producing a disc
# that booted, played the MP2 soundtrack and showed a black screen.
#
# So: stamp the flags into a file, rebuild it only when they actually change,
# and hang every object off it.
# Declared before `all:` below, so say outright which target is the default -
# otherwise the stamp rule, being the first in the file, becomes it.
.DEFAULT_GOAL  := all

FLAGS_STAMP     = build/flags.stamp
FLAGS_STAMP_TEXT = $(CFLAGS) $(ASFLAGS) MBV_STATS=$(MBV_STATS)
.PHONY: force
$(FLAGS_STAMP): force
	@mkdir -p build
	@echo '$(FLAGS_STAMP_TEXT)' | cmp -s - $@ || echo '$(FLAGS_STAMP_TEXT)' > $@

ASSETS          = $(VGM_ASSET) $(MP2_ASSET)
ifneq ($(VIDEO_PLAYER),0)
ASSETS          += $(MBV_ASSET)
endif

all: $(IPLBIN) CD/SYSTEM.BIN $(ASSETS)

$(VGM_ASSET): $(VGM_SOURCE) tools/vgm2fmt.py
	python3 tools/vgm2fmt.py $< $@

$(MP2_ASSET): $(MP2_SOURCE)
	ffmpeg -hide_banner -loglevel error -y -i $< -ac 1 -ar 16000 -c:a mp2 -b:a 32k $@

# The video asset: ffmpeg scales and resamples, tools/mbvenc.c does the codec.
# See docs/MBV_FORMAT.md; -m must not exceed mbvplay.c's MBV_SCRATCH.
MBVFLAGS        ?= -f 12 -g 24 -b 5000 -k 20000 -m 30000
$(MBV_ASSET): $(MBV_SOURCE) build/mbvenc tools/mkmbv.sh
	./tools/mkmbv.sh $< $@ $(MBVFLAGS)

# ---------------------------------------------------------------------
# 32-bit "system" payload (head.S + reloc.c self-relocation + main.c),
# built exactly like fmtowns_playground's mygame_shared: once statically
# (to capture the GOT/relocation layout) then again as a PIC shared
# object (-Bsymbolic), which is what actually gets objcopy'd to a flat
# binary and embedded in the boot image.
# ---------------------------------------------------------------------
# Where the IPL loads the payload (src/boot/defs.h's TSTLOAD, 0x1000:0), and
# the first address that is not main RAM.  On the FM TOWNS the low 768KB are
# RAM and 0xC0000 upwards is the FMR VRAM window, the I/O ROMs and the system
# ROM (TOWNSADDR_FMR_VRAM_BASE in TOWNSEMU's townsdef.h); RAM only resumes at
# 0x100000.  So text, data *and .bss* all have to end below 0xC0000.
#
# This is checked rather than assumed because the failure is silent and
# baffling: .bss that runs over the line is simply not memory.  Writes to it
# land in the VRAM window and read back as whatever the CRTC left there, so
# the program keeps running and merely gets impossible data - a CD ring buffer
# full of 0xFF that the drive never put there, in the case that prompted this.
PAYLOAD_LOAD    = 0x10000
PAYLOAD_LIMIT   = 0xC0000

mygame_shared: $(OBJS) src/boot/mygame_shared.lds Makefile
	$(LD) --warn-constructors --warn-common -static -T src/boot/mygame_shared.lds \
		-o $@ $(OBJS) && \
	$(LD) -shared -Bsymbolic -T src/boot/mygame_shared.lds -o $@ $(OBJS)
	@objdump -h $@ | awk -v base="$(PAYLOAD_LOAD)" -v top="$(PAYLOAD_LIMIT)" \
		'$$2==".bss" { \
		   base = strtonum(base); top = strtonum(top); \
		   last = base + strtonum("0x" $$4) + strtonum("0x" $$3); \
		   printf "payload ends at 0x%x, main RAM ends at 0x%x (%d bytes spare)\n", \
		          last, top, top - last; \
		   if (last > top) { \
		     print "payload overruns main RAM: .bss past 0xC0000 is the FMR VRAM window, not memory" > "/dev/stderr"; \
		     exit 1 } }'

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

src/boot/bootsect.o: src/boot/bootsect.s $(FLAGS_STAMP)
	$(AS) -o $@ $<

src/boot/setup.o: src/boot/setup.s $(FLAGS_STAMP)
	$(AS) -o $@ $<

src/boot/head.o: src/boot/head.s $(FLAGS_STAMP)
	$(AS) $(ASFLAGS) -o $@ $<

src/boot/assets.o: src/boot/assets.S CD/IMAGE.RAW CD/PALETTE.BIN build/PISGA0.spr build/SHTGB0.spr $(FLAGS_STAMP)
	$(AS) -I. -o $@ $<

src/boot/reloc.o: src/boot/reloc.c $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -fno-strict-aliasing -o $@ src/boot/reloc.c

src/main.o: src/main.c $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/main.c

src/common/common.o: src/common/common.c $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/common.c

src/common/palette.o: src/common/palette.c $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/palette.c

src/common/libfmt.o: src/common/libfmt.c src/common/libfmt.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/libfmt.c

src/common/fmt_layers.o: src/common/fmt_layers.c src/common/fmt_layers.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/fmt_layers.c

src/common/fmt_sprite.o: src/common/fmt_sprite.c src/common/fmt_sprite.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/fmt_sprite.c

build/%.spr build/%.h &: assets/%.png tools/png2sprite.py
	mkdir -p build
	python3 tools/png2sprite.py $< build/$*.spr --header build/$*.h

src/common/pad.o: src/common/pad.c src/common/pad.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/pad.c

src/common/scsi.o: src/common/scsi.c src/common/scsi.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/scsi.c

src/common/cdrom.o: src/common/cdrom.c src/common/cdrom.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/cdrom.c

src/common/sound.o: src/common/sound.c src/common/sound.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/sound.c

src/common/iso9660.o: src/common/iso9660.c src/common/iso9660.h src/common/cdrom.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/iso9660.c

src/common/pcmstream.o: src/common/pcmstream.c src/common/pcmstream.h src/common/cdrom.h src/common/iso9660.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/pcmstream.c

build/mp2_synth_table.h: tools/gen_mp2_synth_table.py
	mkdir -p build
	python3 $< $@

build/mp2_synth_window.h: tools/gen_mp2_window.py mp3play/MP3.ASM
	mkdir -p build
	python3 tools/gen_mp2_window.py mp3play/MP3.ASM $@

build/mp2_dct32.h: tools/gen_mp2_dct.py mp3play/MP3.ASM
	mkdir -p build
	python3 tools/gen_mp2_dct.py mp3play/MP3.ASM $@

src/common/mp2.o: src/common/mp2.c src/common/mp2.h build/mp2_synth_table.h build/mp2_synth_window.h build/mp2_dct32.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/mp2.c

src/common/mp2_fast.o: src/common/mp2_fast.S $(FLAGS_STAMP)
	$(AS) -o $@ $<

src/common/dacout.o: src/common/dacout.c src/common/dacout.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/dacout.c

# FMT_MP2_DAC_TICK makes the decoder service the DAC from inside its inner
# loops instead of stalling the output for the length of a frame decode.
src/common/kjmp2_fast.o: src/common/kjmp2_fast.c src/common/kjmp2_fast.h src/common/dacout.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -DFMT_MP2_DAC_TICK -o $@ src/common/kjmp2_fast.c

src/common/mp2stream.o: src/common/mp2stream.c src/common/mp2stream.h src/common/mp2.h src/common/cdrom.h src/common/iso9660.h src/common/dacout.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) $(if $(filter-out 0,$(BUSY_PROBE)),-DFMT_MP2_STATS) -o $@ src/common/mp2stream.c

# FMT_MBV_DAC_TICK makes the frame decoder and the VRAM blit service the DAC
# from inside their loops, the same arrangement kjmp2_fast.c uses: a frame
# takes tens of milliseconds and a sample is due every 62.5us.
src/common/mbv.o: src/common/mbv.c src/common/mbv.h src/common/dacout.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -DFMT_MBV_DAC_TICK -o $@ src/common/mbv.c

src/common/mbvplay.o: src/common/mbvplay.c src/common/mbvplay.h src/common/mbv.h \
                      src/common/mbv_blit.h src/common/dacout.h src/common/libfmt.h \
                      src/common/cdrom.h src/common/iso9660.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -DFMT_MBV_DAC_TICK \
		$(if $(filter-out 0,$(MBV_STATS)),-DFMT_MBV_STATS) -o $@ src/common/mbvplay.c

src/common/cdda.o: src/common/cdda.c src/common/cdda.h src/common/cdrom.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/cdda.c

src/common/vgmplay.o: src/common/vgmplay.c src/common/vgmplay.h src/common/iso9660.h $(FLAGS_STAMP)
	$(CC) -c $(CFLAGS) -o $@ src/common/vgmplay.c

# ---------------------------------------------------------------------
# The IPL: bootsect + setup, which is all the boot ROM ever preloads
# (4 CD sectors / 8192 bytes) and runs. tools/inject_ipl.py writes this
# into the ISO9660 System Area of the finished image and enforces the
# 8192-byte ceiling. The payload rides along separately as CD/SYSTEM.BIN.
# ---------------------------------------------------------------------
$(IPLBIN): src/boot/bootsect.o src/boot/setup.o src/boot/ipl.bin.lds
	$(LD) -T src/boot/ipl.bin.lds src/boot/bootsect.o src/boot/setup.o -o $@

clean:
	rm -f src/boot/*.o src/boot/*.s src/common/*.o src/*.o $(FLAGS_STAMP) \
		mygame_shared mygame_shared.bin CD/SYSTEM.BIN $(IPLBIN) BOOT.BIN \
		src/common/pad.o src/common/scsi.o src/common/cdrom.o src/common/sound.o \
		src/common/iso9660.o src/common/pcmstream.o src/common/dacout.o src/common/cdda.o
	@rm -f $(VGM_ASSET) $(MP2_ASSET) src/common/vgmplay.o src/common/mp2.o src/common/mp2_fast.o src/common/mp2stream.o
	@rm -f src/common/mbv.o src/common/mbvplay.o build/mbvenc build/t_mbv_round build/t_mbv_blit
	@rm -f build/*.spr build/*.h

# ---------------------------------------------------------------------
# Host-side tests.  These build for the host, not the target: the point is
# to check the parts that are pure computation (the decoder) and pure
# arithmetic (the DAC pacing) without needing a Marty in the loop.
# ---------------------------------------------------------------------
HOSTCC          = gcc
TESTFLAGS       = -O2 -Wall -Isrc/common
# The decoder tests build 32-bit so the i386 window kernel is the code
# actually under test, and are compiled -march=i386 like the target.
TEST32FLAGS     = -m32 -march=i386 -Os -fomit-frame-pointer -Wall -Isrc/common

# The encoder is a host tool: it links the target's own decoder primitives
# (src/common/mbv.c) so that its reconstruction is the machine's by
# construction rather than by a second implementation that has to be kept in
# step.  See the head of tools/mbvenc.c.
build/mbvenc: tools/mbvenc.c src/common/mbv.c src/common/mbv.h
	@mkdir -p build
	$(HOSTCC) $(TESTFLAGS) -o $@ tools/mbvenc.c src/common/mbv.c

test: $(MP2_ASSET)
	@mkdir -p build
	@echo "== DAC pacing =="
	$(HOSTCC) $(TESTFLAGS) -Itests/hostio -o build/t_dacpace \
		tests/dacpace_test.c src/common/dacout.c
	@build/t_dacpace
	@echo
	@echo "== MP2 decoder: i386 window kernel vs portable C =="
	$(HOSTCC) $(TEST32FLAGS) -o build/t_mp2_asm tests/mp2_pcm8_test.c src/common/kjmp2_fast.c
	$(HOSTCC) $(TEST32FLAGS) -DFMT_MP2_NO_ASM -o build/t_mp2_c tests/mp2_pcm8_test.c src/common/kjmp2_fast.c
	@build/t_mp2_asm $(MP2_ASSET) build/asm.u8
	@build/t_mp2_c   $(MP2_ASSET) build/c.u8
	@cmp build/asm.u8 build/c.u8 && echo "inline asm output is byte-identical to the C reference"
	@echo
	@echo "== decode/playback interleaving =="
	$(HOSTCC) $(TESTFLAGS) -Itests/hostio -DFMT_MP2_DAC_TICK -o build/t_interleave \
		tests/mp2_interleave_test.c src/common/kjmp2_fast.c src/common/dacout.c
	@build/t_interleave $(MP2_ASSET)
	@echo
	@echo "== MBV: VRAM blit vs libfmt's address transform =="
	$(HOSTCC) $(TESTFLAGS) -o build/t_mbv_blit tests/mbv_blit_test.c
	@build/t_mbv_blit
	@echo
	@echo "== MBV: encoder/decoder agreement =="
	ffmpeg -hide_banner -loglevel error -y -f lavfi \
		-i testsrc2=size=256x240:rate=12:duration=4 \
		-pix_fmt rgb24 -f rawvideo build/mbvclip.rgb
	ffmpeg -hide_banner -loglevel error -y -f lavfi \
		-i "sine=frequency=440:sample_rate=16000:duration=4" \
		-ac 1 -f u8 build/mbvclip.u8
	$(MAKE) --no-print-directory build/mbvenc
	$(HOSTCC) $(TESTFLAGS) -o build/t_mbv_round tests/mbv_roundtrip_test.c \
		src/common/mbv.c -lm
	@build/mbvenc -q -D build/mbvclip.dump build/mbvclip.rgb build/mbvclip.u8 build/mbvclip.mbv
	@build/t_mbv_round build/mbvclip.mbv build/mbvclip.dump build/mbvclip.rgb

.PHONY: all clean test
