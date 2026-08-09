/*
 * FM TOWNS 256x240 @ 15kHz, 8bpp, 256-byte stride background display example.
 *
 * Bare-metal version: boots directly from an FM TOWNS "IPL4" CD-ROM boot
 * sector (src/boot/bootsect.S) through a real-mode -> 32-bit flat
 * protected-mode transition (src/boot/setup.S + src/boot/head.S), with no
 * DOS, no RUN386 DOS extender, and no ISO9660 filesystem driver.
 *
 * The image ("IMAGE.RAW", 256x240 8bpp indexed) and its palette
 * ("PALETTE.BIN", 256 * RGB888) are linked directly into the payload
 * binary (src/boot/assets.S) rather than being loaded from a file at
 * runtime, since there is no filesystem code in this boot path.
 *
 * Video mode setup, palette loading, and the VRAM blit itself are all
 * handled by libfmt (src/common/libfmt.[ch]) - see that header for the
 * list of modes it currently supports.
 */
#include <stdint.h>
#include "test.h"
#include "defs.h"
#include "io.h"
#include "libfmt.h"
#include "fmt_pixel.h"
#include "sound.h"

//----------------------------------------------------------------
// Display geometry
//----------------------------------------------------------------
#define SCREEN_WIDTH        256
#define SCREEN_HEIGHT       240
#define SCREEN_STRIDE       256   /* bytes per VRAM line: LO0(0x20)*8 in single-page mode */
#define IMAGE_SIZE          (SCREEN_WIDTH * SCREEN_HEIGHT)

//----------------------------------------------------------------
// Linked-in image / palette (see src/boot/assets.S)
//----------------------------------------------------------------
extern const uint8_t g_image_data[];
extern const uint8_t g_palette_data[];

//----------------------------------------------------------------
// TEST: a short, audible RF5C68-format triangle-wave PCM sample.
//----------------------------------------------------------------
static uint8_t g_test_tone[2400]; /* 1/8 second at 19.2kHz. */

/* Referenced by src/boot/head.S (CPU id storage + IDT default handler). */
struct cpu_ident cpu_id;

void inter(struct eregs *trap_regs)
{
    (void)trap_regs;
}

//----------------------------------------------------------------
// Entry point, called from src/boot/head.S's startup_32 once flat 32-bit
// protected mode is up and BSS is zeroed.
//----------------------------------------------------------------
void start_main(void)
{
    fmt_set_mode(FMT_MODE_256x240_8BPP);
    fmt_load_palette(g_palette_data, 256);
    fmt_put_image(g_image_data, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_STRIDE);
    fmt_wait_vsync();

    for (uint32_t i = 0; i < sizeof(g_test_tone); i++) {
        uint8_t phase = (uint8_t)(i & 63u);
        uint8_t magnitude = phase < 32u ? phase : (uint8_t)(63u - phase);
        /* RF5C68 data is sign/magnitude: bit 7 set is positive.  Avoid the
         * reserved 0x00 and 0xff codes documented for the chip. */
        g_test_tone[i] = (phase < 32u) ? (uint8_t)(0x80u + magnitude * 3u) :
                                        (uint8_t)(1u + magnitude * 3u);
    }
    fmt_sound_init();
    fmt_sound_set_pan(2, FMT_SOUND_PAN_CENTER);
    fmt_sound_play(2, g_test_tone, sizeof(g_test_tone));

    while (1) {
    }
}
