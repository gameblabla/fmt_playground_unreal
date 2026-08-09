/*
 *	bootsect.S - FM TOWNS "IPL4" CD-ROM boot sector.
 *
 * Based on the memtest86 FM bootsect.s (Copyright (C) 1991,1992 Linus
 * Torvalds; FM TOWNS port by Chris Brady / captainys's FD_IPL), as reused
 * in fmtowns_playground/bootsect.S for floppy boot. This version replaces
 * the floppy CHS load loop with a CD-ROM LBA read, per the FM TOWNS boot
 * ROM's disk BIOS (call far 0xFFFB:0x0014):
 *
 *   - The boot ROM always preloads the first IPL_PRELOAD_BYTES (4 CD
 *     sectors = 8192 bytes) of the disc to linear address 0xB0000 and
 *     transfers control there in real mode (call far BOOTSEG:4).
 *     bootsect.S + setup.S together fit inside that preloaded window, so
 *     no disk read is needed to fetch "setup" separately (unlike the
 *     floppy version, which re-reads it from disk).
 *   - The "system" (our 32-bit payload: head.S + reloc.c + main.c, built
 *     as mygame_shared.bin exactly like fmtowns_playground) is read
 *     explicitly from the CD starting at CD sector PAYLOAD_LBA, for
 *     PAYLOAD_SECTORS 2048-byte sectors, using disk function AH=0x05
 *     (read), device id AL=0xC0 (CD-ROM). This calling convention was
 *     confirmed against a known-good minimal CD IPL test image
 *     (TOWNSEMU/testdata/IPL/CDIMAGE.ISO, disassembled with ndisasm):
 *     AH=5, AL=0xC0, CX=0, DX=<LBA>, BX=<sector count>, DS:DI=<buffer>,
 *     "call $0xfffb, $0x14"; carry set on error.
 *   - Fields at offset 0x20 (4 bytes) / 0x24 (4 bytes) from the IPL4
 *     magic are read by the FM TOWNS UX/Marty ROM disk's chain-loader to
 *     validate this is a real IPL before it lets the boot ROM's normal
 *     IPL4 dispatch run; per docs/HOWFMTOWNS_BOOTS_FROM_CD.txt, they can
 *     just point back at the boot sector itself (start sector 0, length
 *     0 works: verified against the same known-good CD IPL test image).
 */

#include "defs.h"
#include "payload_len.h"

.code16
.section ".bootsect", "ax", @progbits
_boot:

.globl	_main
_main:
	.ascii "IPL4"		/* FM TOWNS IPL Identifier */

.org 4
	jmp	real_start

/* UX/Marty ROM chain-load compatibility fields: point back at ourselves
 * (sector 0, length 0) so the ROM disk's older-IO.SYS patch logic accepts
 * this disc as bootable and falls through to running our IPL, instead of
 * trying to interpret us as a TOWNS OS IO.SYS to patch.
 */
.org 0x20
	.long	0
.org 0x24
	.long	0

.org 0x30
real_start:
	movw	$BOOTSEG, %ax
	movw	%ax, %ds
	movw	$INITSEG, %ax
	movw	%ax, %es

	push	%bx
	call	INITGRAPHICS
	call	VRAMMODE
	call	CLS
	pop	%bx

	/* Relocate the whole boot-ROM-preloaded window (bootsect + setup)
	 * from BOOTSEG to INITSEG, mirroring the classic PC boot-sector
	 * "move myself out of the way" trick. */
	subw	%si, %si
	subw	%di, %di
	movw	$(IPL_PRELOAD_BYTES/2), %cx
	cld
	rep
	movsw
	ljmp	$INITSEG, $go - _boot

go:
	movw	%cs, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	$(0x4000-12), %sp

/*
 * Load the "system" payload (mygame_shared.bin) from the CD-ROM into
 * TSTLOAD, using CD-ROM LBA reads (AH=5, AL=0xC0), chunked at 32 sectors
 * (65536 bytes = exactly one real-mode segment) per BIOS call, matching
 * the chunk size verified to work against a known-good CD IPL image.
 */
load_system:
	movw	$TSTLOAD, %ax
	movw	%ax, %es		/* buffer segment, advances per chunk */
	movw	$PAYLOAD_LBA, %si	/* current LBA, kept in SI across chunks */
	movw	$PAYLOAD_SECTORS, %di	/* sectors remaining */

read_chunk:
	cmpw	$0, %di
	je	load_done

	movw	%di, %cx
	cmpw	$32, %cx
	jbe	1f
	movw	$32, %cx
1:
	pushw	%di
	pushw	%cx

	movw	%es, %ax
	movw	%ax, %ds
	xorw	%di, %di

	movw	$0x0500, %ax		/* AH=5 (read), AL=0xC0 (CD device id) */
	orb	$0xc0, %al
	xorw	%cx, %cx		/* cylinder = 0 (unused for CD/LBA) */
	movw	%si, %dx		/* LBA sector number */
	popw	%bx			/* sector count for this chunk */
	pushw	%bx
	lcall	$0xfffb, $0x0014
	jc	read_error

	popw	%cx			/* sectors read this chunk */
	popw	%di
	subw	%cx, %di		/* sectors remaining -= chunk */
	addw	%cx, %si		/* LBA += chunk */

	/*
	 * Advance the buffer segment by one chunk's worth (32 sectors *
	 * 2048 bytes = 65536 bytes = 0x1000 paragraphs, since a paragraph
	 * is 16 bytes: 0x10000/16 = 0x1000). The previous constant 0x0800
	 * was only half of that (0x8000 bytes = 16 sectors), so the second
	 * and subsequent CD read chunks landed 32768 bytes short of where
	 * they should have, overlapping/clobbering the second half of the
	 * previous chunk instead of appending after it - silently
	 * corrupting everything in the payload past the first ~32KB
	 * (including main.c's CRTC register array and video/palette
	 * tables), even though the first 32KB (code, IDT/GDT setup, and
	 * the start of .rodata) happened to load correctly.
	 */
	movw	%es, %ax
	addw	$0x1000, %ax		/* advance buffer by 32*2048 bytes = 1 segment (0x1000 paragraphs) */
	movw	%ax, %es

	jmp	read_chunk

read_error:
	movb	$0x58, %ah		/* 'X' */
	movw	0x01+0x01*(80*16), %di
	call	DRAWFONT_TALL
error_hlt:
	jmp	error_hlt

load_done:
	movw	%cs, %ax
	movw	%ax, %ds

	/*
	 * Jump to setup.S's entry point. NOTE: we deliberately do *not* use
	 * the fixed "SETUPSEG = INITSEG+0x20" (byte offset 512) convention
	 * from the original floppy-oriented bootsect.S/defs.h: that value
	 * only made sense there because the floppy loader explicitly read
	 * "setup" into memory at that fixed offset via its own disk read.
	 * Here, bootsect.S and setup.S are simply concatenated by
	 * mygame.bin.lds and both relocated verbatim from BOOTSEG to
	 * INITSEG together (see the `rep movsw` above), so setup.S's actual
	 * offset from INITSEG is whatever the linker placed it at -
	 * "start", exported by setup.S. Jumping to the fixed offset 512
	 * instead would land wherever bootsect.S's *actual* assembled size
	 * happens to put us, not necessarily at setup.S's code.
	 */
	ljmp	$INITSEG, $start

.intel_syntax noprefix
INITGRAPHICS:
	MOV		DX,IO_FMR_GVRAMDISPMODE
	MOV		AL,0x27  # Show all planes, write to Page 0 (Page 0 is upper half of VRAM Layer 0)
	OUT		DX,AL

	MOV		DX,IO_FMR_GVRAMMASK
	MOV		AL,0x0F  # Write to all planes
	OUT		DX,AL

	RET

VRAMMODE:
	PUSH	DX
	PUSH	AX
	MOV		AL,0
	JMP		SHORT SET_VRAM_OR_RAM_MODE

SET_VRAM_OR_RAM_MODE:
	MOV		DX,IO_FMR_VRAM_OR_MAINRAM
	OUT		DX,AL
	POP		AX
	POP		DX
	RET

CLS:
	PUSH	ES
	PUSH	DI
	PUSH	AX
	PUSH	CX

	AND		AL,0x0F
	MOV		DX,IO_FMR_GVRAMMASK
	OUT		DX,AL

	MOV		AX,0x0C000
	MOV		ES,AX
	MOV		DI,0
	MOV		CX,80*400/2
	MOV		AX,0
	REP STOSW

	POP		CX
	POP		AX
	POP		DI
	POP		ES
	RET

# DI:  VRAM ADDRESS
# AH:  ASCII Code
# C0000H must be in FM-R VRAM mode.
# BX preserved
# DS preserved
DRAWFONT_TALL:
	PUSH	DS

	MOV		DX,0x0C000
	MOV		DS,DX

	MOV		DX,IO_KVRAM_OR_ANKFONT
	MOV		AL,1
	OUT		DX,AL

	# ANK Font ROM CA000H-
	MOV		AL,8
	MUL		AH
	ADD		AX,0x0A000

	MOV		CX,8
	MOV		SI,AX
drawfont_tall_before:
	MOV		DH,DS:[SI]
	MOV		[DI],DH
	MOV		[DI+0x50],DH
	INC		SI
	ADD		DI,0x0A0
	LOOP	drawfont_tall_before

	MOV		DX,IO_KVRAM_OR_ANKFONT
	XOR		AL,AL
	OUT		DX,AL

	POP		DS
	RET

.att_syntax prefix

/* No 0x55AA "boot signature" is required here: per
 * docs/HOWFMTOWNS_BOOTS_FROM_CD.txt, the FM TOWNS boot ROM only looks for
 * the "IPL4" magic at the very start of the sector, unlike a PC BIOS. */
