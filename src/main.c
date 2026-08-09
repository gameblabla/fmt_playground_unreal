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
 */
#include <stdint.h>
#include "test.h"
#include "defs.h"
#include "io.h"
#include "common.h"
#include "palette.h"

//----------------------------------------------------------------
// Display geometry
//----------------------------------------------------------------
#define SCREEN_WIDTH        256
#define SCREEN_HEIGHT       240
#define SCREEN_STRIDE       256   /* bytes per VRAM line: LO0(0x20)*8 in single-page mode */
#define VRAM_OFFSET         0x0   /* FA0(0)*8 for 8bpp */
#define IMAGE_SIZE          (SCREEN_WIDTH * SCREEN_HEIGHT)

/*
 * FM TOWNS VRAM0 physical base address. This differs between the
 * "full" 386DX/486 FM TOWNS memory map (0x80000000) and the 386SX-based
 * UX/Marty memory map (0xA00000, per TOWNSEMU's
 * TOWNSADDR_386SX_VRAM0_BASE) - this example targets Marty (see run.sh's
 * "-TOWNSTYPE MARTY"), so it uses the 386SX address.
 */
#define TOWNS_VRAM0_BASE_MARTY   0xA00000u

/*
 * 256x240, 15kHz, 8bpp, 256-byte VRAM stride CRTC/video register set.
 * Carried forward from the earlier RUN386-based src/main.c (see git
 * history), which used a 1:1 (non-zoomed) single-page 8bpp mode at
 * 15kHz timing:
 *   - ZOOM=0 -> base x-zoom=1, y-zoom=1
 *   - single-page mode (video[0] bit4 clear) + FO0 != 0 and FO0 != LO0
 *     -> TownsCRTC::GetLowResPageZoom2X() multiplies y-zoom by 2 (not 4),
 *     giving 240 source lines -> 480 physical scanlines (15kHz line
 *     doubling) with 1:1 horizontal pixels.
 *   - LO0=0x0020 -> bytesPerLine = LO0*8 = 256 (single-page mode).
 *   - FA0=0 -> VRAM display starts at byte offset 0.
 *   - HDE0-HDS0=0x200(512) -> monitor width (HDE-HDS)/2 = 256 px.
 *   - VDE0-VDS0=0x0F0(240) -> monitor height (VDE-VDS)*2 = 480 px.
 *
 * NOTE: as of this port, TOWNSEMU screenshots of this register set still
 * show horizontal color banding instead of the full picture; the exact
 * CRTC geometry bug is not yet resolved (see task notes / final report).
 */
static const crtc_set_t crtc = {
   /* 00 HSW1 */ 0x0074, /* 01 HSW2 */ 0x0530, /* 02 ---- */      0, /* 03 ---- */      0,
   /* 04 HST  */ 0x0617, /* 05 VST1 */ 0x000C, /* 06 VST2 */ 0x0018, /* 07 EET  */ 0x0030,
   /* 08 VST  */ 0x049A, /* 09 HDS0 */ 0x00E7, /* 0A HDE0 */ 0x02E7, /* 0B HDS1 */ 0x00E7,
   /* 0C HDE1 */ 0x02E7, /* 0D VDS0 */ 0x0046, /* 0E VDE0 */ 0x0136, /* 0F VDS1 */ 0x0046,
   /* 10 VDE1 */ 0x0136, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x00E7, /* 13 FO0  */ 0x0001,
   /* 14 LO0  */ 0x0020, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x00E7, /* 17 FO1  */ 0x0001,
   /* 18 LO1  */ 0x0020, /* 19 EHAJ */ 0x0056, /* 1A EVAJ */ 0x0007, /* 1B ZOOM */ 0x0000,
   /*
    * CR0 bits[1:0] select page 0's color depth (TownsCRTC::
    * GetPageBitsPerPixel: in single-page mode, CL==3 -> 8bpp,
    * CL==2 -> 16bpp). The previous value 0x002A has bits[1:0]=2,
    * which made the CRTC read our 8bpp-indexed VRAM as 16bpp pixels
    * (turning small palette-index bytes into near-black 16-bit
    * colors) - this was the actual cause of the solid black screen.
    * 0x002B sets bits[1:0]=3 for correct 8bpp page-0 decoding.
    */
   /* 1C CR0  */ 0x002B, /* 1D CR1  */ 0x0001, /* 1E FR   */ 0x0002, /* 1F CR2  */ 0x0188
};

/* video[0] = 0x0F: single-page mode (bit4 clear), CL=3 (8bpp) */
static const video_set_t video = { 0x0f, 0x09 };

//----------------------------------------------------------------
// Linked-in image / palette (see src/boot/assets.S)
//----------------------------------------------------------------
extern const uint8_t g_image_data[];
extern const uint8_t g_palette_data[];

/* Referenced by src/boot/head.S (CPU id storage + IDT default handler). */
struct cpu_ident cpu_id;

void inter(struct eregs *trap_regs)
{
    (void)trap_regs;
}

//----------------------------------------------------------------
// VRAM blit
//----------------------------------------------------------------
static void Put_Image(const uint8_t *src, int width, int height, int stride)
{
    volatile uint8_t *vram = (volatile uint8_t *)(TOWNS_VRAM0_BASE_MARTY + VRAM_OFFSET);

    for (int y = 0; y < height; y++) {
        const uint8_t *srow = src + y * width;
        volatile uint8_t *drow = vram + y * stride;
        for (int x = 0; x < width; x++) {
            drow[x] = srow[x];
        }
    }
}

static void load_palette(const uint8_t *data)
{
    for (int i = 0; i < 256; i++) {
        uint8_t r = data[i * 3];
        uint8_t g = data[i * 3 + 1];
        uint8_t b = data[i * 3 + 2];
        set_palette((uint8_t)i, r, g, b);
    }
}

//----------------------------------------------------------------
// CRTC helpers used to sync display start after mode change
//----------------------------------------------------------------
static void WaitforVsync(void)
{
    outb(30, 0x0440);
    while (!(inb(0x443) & 4)) {
    }
}

//----------------------------------------------------------------
// Entry point, called from src/boot/head.S's startup_32 once flat 32-bit
// protected mode is up and BSS is zeroed.
//----------------------------------------------------------------
void start_main(void)
{
    /* Make sure GVRAM (not main RAM) is mapped at the VRAM window. */
    outb(0, IO_FMR_VRAM_OR_MAINRAM);

    stop_display();
    set_crtc(crtc);
    set_video(video);

    load_palette(g_palette_data);

    start_display();

    Put_Image(g_image_data, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_STRIDE);

    WaitforVsync();

    while (1) {
    }
}
