/* Native FM TOWNS packed-pixel drawing verification. */
#include <stdint.h>
#include "test.h"
#include "defs.h"
#include "libfmt.h"
#include "fmt_pixel.h"
#include "fmt_layers.h"
#include "fmt_sprite.h"
#include "vgmplay.h"
#include "mp2stream.h"
#include "io.h"
#include "dacout.h"

struct cpu_ident cpu_id;
struct eregs;
extern const uint8_t g_sprite_pisga0[];
extern const uint8_t g_sprite_shtgb0[];

void inter(struct eregs *trap_regs)
{
    (void)trap_regs;
}

#if !FMT_VGM_PLAYER && FMT_GFX_TEST == 0 && FMT_TEST_256x240
static void draw_256x240_test(void)
{
#if FMT_PIXEL_TEST_BPP == 15
    static uint16_t frame[256 * 240];
    const uint16_t red = rgb15(255, 0, 0);
    const uint16_t green = rgb15(0, 255, 0);
    const uint16_t blue = rgb15(0, 0, 255);
    const uint16_t white = rgb15(255, 255, 255);

    fmt_set_mode(FMT_MODE_256x240_16BPP);
    for (uint16_t y = 0; y < 240; ++y) {
        for (uint16_t x = 0; x < 256; ++x) {
            uint16_t color = ((x >> 5) ^ (y >> 5)) & 1 ? red : green;
            if (x >= 80 && x < 176 && y >= 80 && y < 160) {
                color = blue;
            }
            if (x == 0 || x == 255 || y == 0 || y == 239) {
                color = white;
            }
            frame[(uint32_t)y * 256u + x] = color;
        }
    }
    fmt_put_image(frame, 256, 240, 512);
#else
    static uint8_t frame[256 * 240];
    static const uint8_t palette[5 * 3] = {
          0,   0,   0,
        255,   0,   0,
          0, 255,   0,
          0,   0, 255,
        255, 255, 255
    };

    fmt_set_mode(FMT_MODE_256x240_8BPP);
    fmt_load_palette(palette, 5);
    for (uint16_t y = 0; y < 240; ++y) {
        for (uint16_t x = 0; x < 256; ++x) {
            uint8_t color = ((x >> 5) ^ (y >> 5)) & 1 ? 1 : 2;
            if (x >= 80 && x < 176 && y >= 80 && y < 160) {
                color = 3;
            }
            if (x == 0 || x == 255 || y == 0 || y == 239) {
                color = 4;
            }
            frame[(uint32_t)y * 256u + x] = color;
        }
    }
    fmt_put_image(frame, 256, 240, 256);
#endif
    fmt_flip_page();
}
#endif

#if !FMT_VGM_PLAYER && FMT_GFX_TEST == 0 && !FMT_TEST_256x240 && FMT_PIXEL_TEST_BPP != 15
static void draw_8bpp_test(void)
{
    static const uint8_t palette[5 * 3] = {
          0,   0,   0,
        255,   0,   0,
          0, 255,   0,
          0,   0, 255,
        255, 255, 255
    };

    fmt_set_mode(FMT_MODE_320x240_8BPP);
    fmt_load_palette(palette, 5);

    if (!fmt_page_flipping_available() || fmt_draw_page() != 1) {
        while (1) {
        }
    }

    /* Begin with an explicitly black 320x240 screen. */
    for (uint16_t y = 0; y < 240; ++y) {
        for (uint16_t x = 0; x < 320; x += 4) {
            Set_4Pixels_320x240_8bpp(x, y, 0);
        }
    }
    fmt_wait_vsync();

    /* 304x48 bars exercise 4-, 2-, and masked 1-pixel transfers. */
    for (uint16_t y = 8; y < 56; ++y) {
        for (uint16_t x = 8; x < 312; x += 4) {
            Set_4Pixels_320x240_8bpp(x, y, 0x01010101u);
        }
    }
    for (uint16_t y = 64; y < 112; ++y) {
        for (uint16_t x = 8; x < 312; x += 2) {
            Set_2Pixels_320x240_8bpp(x, y, 2, 2);
        }
    }
    for (uint16_t y = 120; y < 168; ++y) {
        for (uint16_t x = 8; x < 312; ++x) {
            Set_Pixel_320x240_8bpp(x, y, 3);
        }
    }
    for (uint16_t x = 0; x < 320; ++x) {
        Set_Pixel_320x240_8bpp(x, 0, 4);
        Set_Pixel_320x240_8bpp(x, 239, 4);
    }
    for (uint16_t y = 0; y < 240; ++y) {
        Set_Pixel_320x240_8bpp(0, y, 4);
        Set_Pixel_320x240_8bpp(319, y, 4);
    }

    /* Nothing above touched the visible page.  Reveal the completed frame
     * during vertical blank and make the old black page the draw target. */
    fmt_flip_page();
}
#endif

#if !FMT_VGM_PLAYER && FMT_GFX_TEST == 1
static void dual_bg_test(void)
{
    fmt_bg_layer_t bg0={320,240,640};
    fmt_bg_layer_t bg1={256,160,512};
    if(fmt_set_two_backgrounds(bg0,bg1,1)) while(1){}
    for(uint16_t y=0;y<240;y++) for(uint16_t x=0;x<320;x++)
        fmt_bg_put_pixel(0,x,y,rgb15((x>>4)&31,(y>>3)&31,8));
    fmt_bg_clear(1,0x8000);
    for(uint16_t y=24;y<136;y++) for(uint16_t x=24;x<232;x++) {
        if(x<32||x>=224||y<32||y>=128)
            fmt_bg_put_pixel(1,x,y,rgb15(31,2,2));
    }
}
#endif

#if !FMT_VGM_PLAYER && FMT_GFX_TEST == 2
static void sprite_test(void)
{
    fmt_bg_layer_t bg={256,240,512};
    if(fmt_set_background_and_sprites(bg)) while(1){}
    for(uint16_t y=0;y<240;y++) for(uint16_t x=0;x<256;x++)
        fmt_bg_put_pixel(0,x,y,rgb15(2,(x>>3)&31,(y>>3)&31));

    fmt_sprite_init();
    fmt_sprite_load_15bpp(128,g_sprite_pisga0,16);
    fmt_sprite_load_15bpp(192,g_sprite_shtgb0,64);
    uint16_t idx=1023;
    for(uint16_t ty=0;ty<4;ty++) for(uint16_t tx=0;tx<4;tx++,idx--)
        fmt_sprite_set(idx,24+tx*16,26+ty*16,128+(ty*4+tx)*4,0,0,0);
    for(uint16_t ty=0;ty<8;ty++) for(uint16_t tx=0;tx<8;tx++,idx--)
        fmt_sprite_set(idx,112+tx*16,74+ty*16,192+(ty*8+tx)*4,0,0,0);
    fmt_sprite_enable(idx+1);
}
#endif

#if !FMT_VGM_PLAYER && FMT_GFX_TEST == 0 && !FMT_TEST_256x240 && FMT_PIXEL_TEST_BPP == 15
static void draw_15bpp_test(void)
{
    const uint16_t red = rgb15(255, 0, 0);
    const uint16_t green = rgb15(0, 255, 0);
    const uint16_t blue = rgb15(0, 0, 255);
    const uint16_t white = rgb15(255, 255, 255);

    fmt_set_mode(FMT_MODE_320x240_16BPP);

    if (!fmt_page_flipping_available() || fmt_draw_page() != 1) {
        while (1) {
        }
    }

    /* Begin this mode with black too, using the fast two-pixel path. */
    for (uint16_t y = 0; y < 240; ++y) {
        for (uint16_t x = 0; x < 320; x += 2) {
            Set_2Pixels_320x240_15bpp(x, y, 0, 0);
        }
    }
    fmt_wait_vsync();

    for (uint16_t y = 8; y < 80; ++y) {
        for (uint16_t x = 8; x < 312; x += 2) {
            Set_2Pixels_320x240_15bpp(x, y, red, green);
        }
    }
    for (uint16_t y = 88; y < 168; ++y) {
        for (uint16_t x = 8; x < 312; ++x) {
            Set_Pixel_320x240_15bpp(x, y, blue);
        }
    }
    for (uint16_t x = 0; x < 320; ++x) {
        Set_Pixel_320x240_15bpp(x, 0, white);
        Set_Pixel_320x240_15bpp(x, 239, white);
    }
    for (uint16_t y = 0; y < 240; ++y) {
        Set_Pixel_320x240_15bpp(0, y, white);
        Set_Pixel_320x240_15bpp(319, y, white);
    }

    fmt_flip_page();
}
#endif

#if FMT_YM_BUSY_PROBE
/* In-machine measurement of the three timing paths this project depends on,
 * left at physical 0x00080000 - above this payload, which loads at 0x10000 and
 * is under 256 KiB - so it can be read with `MEMDUMP PHYS:00080000 24 1` in
 * the emulator console.
 *
 * Layout, all little-endian 32-bit after the magic:
 *   0  "YMBZ"
 *   4  microseconds for 1000 writes to the 1us wait port 0x6C
 *   8  microseconds for 1000 busy-flag-paced DAC writes (dac_pcm.c's loop)
 *  12  microseconds for 1000 timer-paced DAC writes at FMT_DAC_RATE
 *  16  times the free-running counter went backwards in 20000 reads
 *  20  0xA5A5A5A5 trailer
 *
 * Elapsed time is accumulated as 16-bit differences so the counter's 65.536ms
 * wrap does not matter.
 *
 * Build with `make BUSY_PROBE=1`. This is a diagnostic, not part of the demo.
 */
#define PROBE_RESULTS ((volatile uint32_t *)0x00080000u)
#define PROBE_ITERS   1000u

static void probe_elapsed_step(uint32_t *acc, uint16_t *prev)
{
    uint16_t now = inw(FMT_DAC_FREERUN_TIMER);
    *acc += (uint16_t)(now - *prev);
    *prev = now;
}

static uint8_t g_probe_pcm[PROBE_ITERS];

static void ym_busy_probe(void)
{
    uint32_t wait_us = 0, busy_us = 0, timer_us = 0, backwards = 0;
    uint16_t prev, now, last;
    unsigned i;

    for (i = 0; i < PROBE_ITERS; ++i) {
        g_probe_pcm[i] = (uint8_t)((i & 16u) ? 168 : 88);
    }

    outb(0x03, FMT_DAC_SOUND_MUTE);
    outb(0x7f, FMT_DAC_SOUND_AUDIO);
    outb(FMT_DAC_YM_REG_DAC_ENABLE, FMT_DAC_YM_ADDR0);
    outb(0x80, FMT_DAC_YM_DATA0);
    while (inb(FMT_DAC_YM_ADDR0) & FMT_DAC_YM_BUSY) {
    }
    outb(FMT_DAC_YM_REG_DAC_DATA, FMT_DAC_YM_ADDR0);

    /* 1. Does the free-running counter advance sanely at all? Time a known
     *    number of 1us waits against it. */
    prev = inw(FMT_DAC_FREERUN_TIMER);
    for (i = 0; i < PROBE_ITERS; ++i) {
        outb(0, 0x6C);
        probe_elapsed_step(&wait_us, &prev);
    }

    /* 2. Is it monotonic? */
    last = inw(FMT_DAC_FREERUN_TIMER);
    for (i = 0; i < 20000u; ++i) {
        now = inw(FMT_DAC_FREERUN_TIMER);
        if ((int16_t)(now - last) < 0) {
            backwards++;
        }
        last = now;
    }

    /* 3. dac_pcm.c's loop: one wait, one DAC write, spin on the busy flag.
     *    This is the loop confirmed to reach 32000Hz on real hardware. */
    prev = inw(FMT_DAC_FREERUN_TIMER);
    for (i = 0; i < PROBE_ITERS; ++i) {
        outb(0, 0x6C);
        outb(g_probe_pcm[i], FMT_DAC_YM_DATA0);
        while (inb(FMT_DAC_YM_ADDR0) & FMT_DAC_YM_BUSY) {
        }
        probe_elapsed_step(&busy_us, &prev);
    }

    /* 4. The shipping timer-paced path, driven exactly as mp2stream does. */
    fmt_dac_start();
    fmt_dac_submit(g_probe_pcm, PROBE_ITERS);
    prev = inw(FMT_DAC_FREERUN_TIMER);
    while (!fmt_dac_drained()) {
        fmt_dac_tick();
        probe_elapsed_step(&timer_us, &prev);
    }
    fmt_dac_stop();

    /* 5. Emit a steady 500Hz square wave through the same timer-paced path,
     *    forever, so the emulator's own audio recorder can be pointed at a
     *    signal whose shape is known exactly. Build with BUSY_PROBE=2. */
#if FMT_YM_BUSY_PROBE == 2
    fmt_dac_start();
    for (;;) {
        fmt_dac_submit(g_probe_pcm, PROBE_ITERS);
        while (!fmt_dac_drained()) {
            fmt_dac_tick();
        }
    }
#endif

    PROBE_RESULTS[0] = 0x5A424D59u;   /* "YMBZ" */
    PROBE_RESULTS[1] = wait_us;
    PROBE_RESULTS[2] = busy_us;
    PROBE_RESULTS[3] = timer_us;
    PROBE_RESULTS[4] = backwards;
    PROBE_RESULTS[5] = 0xA5A5A5A5u;

    while (1) {
    }
}
#endif

void start_main(void)
{
#if FMT_YM_BUSY_PROBE
    ym_busy_probe();
#elif FMT_VGM_PLAYER
    if (fmt_vgm_load_file("MUSIC.FTV") == 0) {
        fmt_vgm_play();
        fmt_vgm_stop();
    }
#elif FMT_MP2_PLAYER
    if (fmt_mp2_stream_load_file("MUSIC.MP2") == 0) {
        fmt_mp2_stream_play_streaming();
        fmt_mp2_stream_stop();
    }
#elif FMT_GFX_TEST == 1
    dual_bg_test();
#elif FMT_GFX_TEST == 2
    sprite_test();
#elif FMT_TEST_256x240
    draw_256x240_test();
#elif FMT_PIXEL_TEST_BPP == 15
    draw_15bpp_test();
#else
    draw_8bpp_test();
#endif
    while (1) {
        fmt_wait_vsync();
    }
}
