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
 *   - ZOOM's low nibble (page 0 X-zoom) = 1 -> TownsCRTC::
 *     GetLowResPageZoom2X() computes zoom.x()=(ZOOM&15)+1 in *2 fixed
 *     point, i.e. raw field value N means an (N+1)/2 actual multiplier -
 *     so N=0 is a *0.5* horizontal zoom (half-width, black right half of
 *     the frame, exactly what earlier TOWNSEMU screenshots of this
 *     register set showed) and N=1 is the 1.0/"1:1 pixels" we actually
 *     want.
 *   - LO0=0x0020 -> bytesPerLine = LO0*8 = 256 (single-page mode).
 *   - FA0=0 -> VRAM display starts at byte offset 0.
 *   - HDE0-HDS0=0x200(512) -> monitor width (HDE-HDS)/2 = 256 px.
 *   - VDE0-VDS0=0x0F0(240) -> monitor height (VDE-VDS)*2 = 480 px.
 *
 * NOTE on Y ("*2 for 15kHz" / "line-doubling"): an earlier version of
 * this comment claimed FO0!=0 && FO0!=LO0 makes
 * TownsCRTC::GetLowResPageZoom2X() double the Y zoom to turn a 0.5
 * baseline into 1.0 for 480 physical scanlines. That is not what
 * happens at render time: with these FO0/LO0 values,
 * TownsRender::Render8Bit (TOWNSEMU/src/towns/render/render.cpp) ends
 * up with ZV=layer.zoom2x.y()/2=1, meaning each of our 240 source rows
 * maps to exactly ONE physical screen row (verified empirically - see
 * verify_pixels.py, 0% mismatch treating screen y = top_y + row, NOT
 * top_y + 2*row). "Display Size:(256,480)" in TOWNSEMU's PRINT CRTC
 * output is the CRTC's scan-geometry bound, not a guarantee the layer
 * gets vertically duplicated to fill it - we simply draw into the top
 * 240 of those 480 rows and leave the rest black. Getting genuine
 * 2x row-doubling would require zoom2x.y()=4 (ZV=2), which the FO0==0
 * or FO0==LO0 branch of GetLowResPageZoom2X provides, but that same
 * condition also halves GetPageSizeOnMonitor's height, and the two
 * effects cancel out coverage-wise rather than compounding - not worth
 * chasing for this example, since a static top-aligned 256x240 image
 * is exactly what we want.
 */
static const crtc_set_t crtc = {
   /* 00 HSW1 */ 0x0074, /* 01 HSW2 */ 0x0530, /* 02 ---- */      0, /* 03 ---- */      0,
   /* 04 HST  */ 0x0617, /* 05 VST1 */ 0x000C, /* 06 VST2 */ 0x0018, /* 07 EET  */ 0x0030,
   /* 08 VST  */ 0x049A, /* 09 HDS0 */ 0x00E7, /* 0A HDE0 */ 0x02E7, /* 0B HDS1 */ 0x00E7,
   /* 0C HDE1 */ 0x02E7, /* 0D VDS0 */ 0x0046, /* 0E VDE0 */ 0x0136, /* 0F VDS1 */ 0x0046,
   /* 10 VDE1 */ 0x0136, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x00E7, /* 13 FO0  */ 0x0001,
   /* 14 LO0  */ 0x0020, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x00E7, /* 17 FO1  */ 0x0001,
   /* 18 LO1  */ 0x0020, /* 19 EHAJ */ 0x0056, /* 1A EVAJ */ 0x0007, /* 1B ZOOM */ 0x0001,
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

/*
 * video[1] (sifter[1]) bits[5:4] select which palette bank the analog
 * palette I/O ports (0xFD90-0xFD96) actually write to: PLT==0 or 2 route
 * writes to the 16-color palette (TownsCRTC::AnalogPalette::Set16),
 * PLT==1 or 3 route to the 256-color palette (Set256) that 8bpp page 0
 * actually reads from (see TOWNSEMU's crtcbase.cpp SetRed/Green/Blue).
 * The previous value 0x09 has PLT=(0x09>>4)&3=0, so load_palette()'s
 * writes were silently landing in the unused 16-color bank, leaving the
 * real 256-color palette at its uninitialized/garbage reset state -
 * this was the actual cause of the black screen (VRAM image data and
 * CRTC geometry were both already correct). 0x18 (PLT=1) matches the
 * known-working register set in display_settings_8bit.txt.
 */
static const video_set_t video = { 0x0A, 0x18 };

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
/*
 * Low-res single-page 8bpp VRAM is NOT a simple linear/row-major byte
 * array on real FM TOWNS hardware (and TOWNSEMU faithfully reproduces
 * this): TownsRender::BuildImage() (TOWNSEMU/src/towns/render/render.cpp)
 * renders single-page mode through VRAM1Trans (TOWNSEMU/src/towns/
 * render/render.h), which remaps every "logical" scanline byte offset
 * (row*bytesPerLine + column, exactly what a naive linear blit would
 * use) through:
 *     phys = ((off&4)<<16) | ((off&0x7FFF8)>>1) | (off&3)
 * before reading VRAM. This is the real single-page-mode VRAM
 * addressing (bit 2 of the logical offset selects which of the two
 * 256KB VRAM planes a 4-byte group lives in, and the rest of the
 * address is halved/packed accordingly) - it is not an emulator quirk
 * or a CRTC register misconfiguration.
 *
 * A plain row-major Put_Image (writing srow[x] to vram[y*stride+x])
 * writes to the *logical* offset directly instead of its transformed
 * physical address, so the renderer ends up sampling 4-byte chunks
 * from the wrong interleaved location - this produced the vertical
 * comb/stripe artifact seen on screen even though a raw VRAM memory
 * dump (SAVEMEMDUMP, which reads linearly and bypasses VRAM1Trans)
 * matched IMAGE.RAW byte-for-byte.
 *
 * The fix: apply the same forward transform when writing, so the byte
 * for logical offset N lands at the physical address the renderer will
 * look it up from (verified bijective/collision-free for our 256x240
 * image's offset range 0..61439, max physical address 0x477FF, well
 * inside VRAM0).
 */
static inline uint32_t vram_singlepage_trans(uint32_t off)
{
    return ((off & 4u) << 16) | ((off & 0x7fff8u) >> 1) | (off & 3u);
}

static void Put_Image(const uint8_t *src, int width, int height, int stride)
{
    volatile uint8_t *vram = (volatile uint8_t *)(TOWNS_VRAM0_BASE_MARTY + VRAM_OFFSET);

    for (int y = 0; y < height; y++) {
        const uint8_t *srow = src + y * width;
        for (int x = 0; x < width; x++) {
            uint32_t off = (uint32_t)y * (uint32_t)stride + (uint32_t)x;
            vram[vram_singlepage_trans(off)] = srow[x];
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
