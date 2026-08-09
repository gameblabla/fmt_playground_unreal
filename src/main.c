/*
 * FM TOWNS 256x240 @ 15kHz, 8bpp, 256-byte stride background display example.
 *
 * Runs under MSDOS via the RUN386 DOS extender (32-bit flat protected mode,
 * no manual "unreal mode" transition is required or possible under RUN386 -
 * the extender already provides flat addressing through GDT selectors).
 *
 * Loads a raw 8bpp indexed image ("IMAGE.RAW", 256x240) and its palette
 * ("PALETTE.BIN", 256 * RGB888) from the CD-ROM, programs the CRTC/video
 * registers for 256x240 15kHz 8bpp mode with a 256-byte VRAM stride, and
 * blits the image into VRAM.
 */
#include <stdio.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stddef.h>
#include <inttypes.h>
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
 * 256x240, 15kHz, 8bpp, 256-byte VRAM stride CRTC/video register set.
 * Based on the CRTC_SET_256240_512256 table from the earlier demo
 * revisions (src/back.old, src/last.old), which gives a 1:1 (non-zoomed)
 * 256x240 display; the frame/line-offset fields (FO0/LO0, FO1/LO1) are
 * changed from the original 512-byte VRAM line stride down to 256 bytes
 * (LO0=LO1=0x0020, i.e. 0x20 * 8 = 256 bytes/line) per
 * display_settings_stride256.txt. The CRTC_SET_STRIDE_256 table in
 * display_settings_stride256.txt itself is a zoomed x8 "Wolfenstein-style"
 * viewport mode, not a 1:1 256x240 background, so it is not used here.
 */
/*
 * Single-page 8bpp mode at "15kHz" HST timing (HST=0x0617, matching
 * display_settings_stride256.txt), with:
 *   - ZOOM=0 -> base x-zoom=1, y-zoom=1
 *   - single-page mode (video[0] bit4 clear) + FO0 != 0 and FO0 != LO0
 *     -> crtc.cpp GetLowResPageZoom2X() multiplies y-zoom by 2 (not 4),
 *     giving the expected 15kHz 2x line-doubled output (240 source lines
 *     -> 480 physical scanlines) with 1:1 horizontal pixels.
 *   - LO0=0x0020 -> bytesPerLine = LO0*8 = 256 (single-page mode doubles
 *     the *4 multiplier used in two-page mode; see GetPageBytesPerLine()).
 *   - FA0=0 -> VRAM display starts at byte offset 0 (FA0*8 for 8bpp).
 *   - HDE0-HDS0=0x200(512) -> monitor width (HDE-HDS)/2 = 256 px.
 *   - VDE0-VDS0=0x0F0(240) -> monitor height (VDE-VDS)*2 = 480 px.
 */
static const crtc_set_t crtc = {
   /* 00 HSW1 */ 0x0074, /* 01 HSW2 */ 0x0530, /* 02 ---- */      0, /* 03 ---- */      0,
   /* 04 HST  */ 0x0617, /* 05 VST1 */ 0x000C, /* 06 VST2 */ 0x0018, /* 07 EET  */ 0x0030,
   /* 08 VST  */ 0x049A, /* 09 HDS0 */ 0x00E7, /* 0A HDE0 */ 0x02E7, /* 0B HDS1 */ 0x00E7,
   /* 0C HDE1 */ 0x02E7, /* 0D VDS0 */ 0x0046, /* 0E VDE0 */ 0x0136, /* 0F VDS1 */ 0x0046,
   /* 10 VDE1 */ 0x0136, /* 11 FA0  */ 0x0000, /* 12 HAJ0 */ 0x00E7, /* 13 FO0  */ 0x0001,
   /* 14 LO0  */ 0x0020, /* 15 FA1  */ 0x0000, /* 16 HAJ1 */ 0x00E7, /* 17 FO1  */ 0x0001,
   /* 18 LO1  */ 0x0020, /* 19 EHAJ */ 0x0056, /* 1A EVAJ */ 0x0007, /* 1B ZOOM */ 0x0000,
   /* 1C CR0  */ 0x002A, /* 1D CR1  */ 0x0001, /* 1E FR   */ 0x0002, /* 1F CR2  */ 0x0188
};

/* video[0] = 0x0F: single-page mode (bit4 clear), CL=3 (8bpp) */
static const video_set_t video = { 0x0f, 0x09 };

//----------------------------------------------------------------
// CPU cache
//----------------------------------------------------------------
static inline int cpu_has_cache(void)
{
    uint32_t original, modified, after;

    __asm__ volatile ("pushfl\n\tpopl %0" : "=r" (original));
    modified = original ^ 0x40000;
    __asm__ volatile ("pushl %0\n\tpopfl" : : "r" (modified));
    __asm__ volatile ("pushfl\n\tpopl %0" : "=r" (after));

    return ((after ^ original) & 0x40000) != 0;
}

void set_cache(int val)
{
    if (!cpu_has_cache())
        return;

    switch (val) {
        case 0: cache_off(); break;
        case 1: cache_on();  break;
    }
}

void sleep(int n)
{
    int i;
    for (i = 0; i < n * 1000; i++)
        outb(0, 0x006C);
}

//----------------------------------------------------------------
// DOS extender file I/O (int 0x21)
//----------------------------------------------------------------
static inline uint16_t dosext_open_handle(uint8_t mode, const char *filename, uint16_t *fd)
{
    uint16_t result;
    bool cf;

    asm volatile (
        "movb    $0x3d, %%ah\n\t"
        "int     $0x21"
        : "=@ccc" (cf), "=a" (result)
        : "a" (mode), "d" (filename), "m" (*(const char (*)[])filename)
    );

    if (!cf) {
        *fd = result;
        return 0;
    }
    return result;
}

static inline bool dosext_close_handle(uint16_t handle, uint16_t *r)
{
    uint16_t result;
    bool cf;

    asm volatile (
        "movb    $0x3e, %%ah\n\t"
        "int     $0x21"
        : "=@ccc" (cf), "=a" (result)
        : "b" (handle)
    );

    *r = result;
    return !cf;
}

static inline bool dosext_read_handle(uint16_t handle, uint32_t count, void *buf, uint32_t *r)
{
    uint16_t result;
    bool cf;

    asm volatile (
        "movb    $0x3f, %%ah\n\t"
        "int     $0x21"
        : "=@ccc" (cf), "=a" (result), "=m" (*(char (*)[])buf)
        : "b" (handle), "c" (count), "d" (buf)
    );

    if (cf) {
        *r = (uint16_t)result;
    } else {
        *r = result;
    }
    return !cf;
}

//----------------------------------------------------------------
// Image / palette loading
//----------------------------------------------------------------
static uint8_t picture[IMAGE_SIZE];

void load_raw_image(const char *filename)
{
    uint16_t file_handle;
    if (dosext_open_handle(0, filename, &file_handle) != 0)
        return;

    uint32_t bytes_read;
    dosext_read_handle(file_handle, IMAGE_SIZE, picture, &bytes_read);

    uint16_t close_err;
    dosext_close_handle(file_handle, &close_err);
}

void load_palette(const char *filename)
{
    const size_t palette_entries = 256;
    const size_t expected_size = palette_entries * 3;

    uint16_t file_handle;
    if (dosext_open_handle(0, filename, &file_handle) != 0)
        return;

    uint8_t palette_data[expected_size];
    uint32_t bytes_read;
    if (!dosext_read_handle(file_handle, expected_size, palette_data, &bytes_read) ||
        bytes_read != expected_size) {
        uint16_t close_dummy;
        dosext_close_handle(file_handle, &close_dummy);
        return;
    }

    uint16_t close_err;
    dosext_close_handle(file_handle, &close_err);

    for (size_t i = 0; i < palette_entries; i++) {
        uint8_t r = palette_data[i * 3];
        uint8_t g = palette_data[i * 3 + 1];
        uint8_t b = palette_data[i * 3 + 2];
        set_palette(i, r, g, b);
    }
}

//----------------------------------------------------------------
// VRAM blit
//----------------------------------------------------------------
__seg_gs uint8_t *vram8 = 0;

static void Put_Image(const uint8_t *src, int width, int height, int stride)
{
    uint16_t vram_selector = 0x10c;
    asm volatile("movw %w0, %%gs\n\t" : : "r"(vram_selector));
    vram8 = (__seg_gs uint8_t *)VRAM_OFFSET;

    for (int y = 0; y < height; y++) {
        const uint8_t *srow = src + y * width;
        __seg_gs uint8_t *drow = vram8 + y * stride;
        for (int x = 0; x < width; x++) {
            drow[x] = srow[x];
        }
    }
}

//----------------------------------------------------------------
// CRTC helpers used to sync display start after mode change
//----------------------------------------------------------------
static void WaitforVsync(void)
{
    __outb(30, 0x0440);
    while (!(__inb(0x443) & 4)) {
    }
}

//----------------------------------------------------------------
// Entry point
//----------------------------------------------------------------
int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    set_cache(1);

    stop_display();
    set_crtc(crtc);
    set_video(video);

    load_palette("PALETTE.BIN");

    start_display();

    load_raw_image("IMAGE.RAW");

    Put_Image(picture, SCREEN_WIDTH, SCREEN_HEIGHT, SCREEN_STRIDE);

    WaitforVsync();

    while (1) {
    }

    return 0;
}
