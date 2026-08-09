OUTPUT_FORMAT("binary")
OUTPUT_ARCH("i386")

ENTRY(_main);
SECTIONS {
	. = 0;
	.bootsect : { *(.bootsect) }
	.setup : { *(.setup) }
	/*
	 * Pad the combined bootsect+setup image out to exactly 8192 bytes
	 * (IPL_PRELOAD_BYTES, i.e. PAYLOAD_LBA*2048 - see defs.h and the
	 * Makefile's payload_len.h generation) before the "system" payload
	 * (mygame_shared.bin, pulled in below via `*(.data)` from the
	 * `-b binary` linker input) begins. Without this, the payload would
	 * be appended right after .setup's actual end (bootsect.S + setup.S
	 * together are only ~2333 bytes, nowhere near a CD-sector boundary),
	 * while bootsect.S's CD read code unconditionally reads the
	 * "system" starting at CD sector PAYLOAD_LBA=4 (byte 8192) - a
	 * mismatch that silently reads the wrong CD sectors into memory.
	 */
	. = 8192;
	.memtest : {
		_start = . ;
		*(.data)
		_end = . ;
	}
	_syssize = (_end - _start + 15) >> 4;
	
	/DISCARD/ : { *(.note*) *(.comment) }
}

