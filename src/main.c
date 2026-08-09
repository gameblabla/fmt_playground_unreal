/* Native FM TOWNS packed-pixel drawing verification. */
#include <stdint.h>
#include "test.h"
#include "defs.h"
#include "libfmt.h"
#include "fmt_pixel.h"

struct cpu_ident cpu_id;
struct eregs;

void inter(struct eregs *trap_regs)
{
    (void)trap_regs;
}

#if FMT_PIXEL_TEST_BPP != 15
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
}
#endif

#if FMT_PIXEL_TEST_BPP == 15
static void draw_15bpp_test(void)
{
    const uint16_t red = rgb15(255, 0, 0);
    const uint16_t green = rgb15(0, 255, 0);
    const uint16_t blue = rgb15(0, 0, 255);
    const uint16_t white = rgb15(255, 255, 255);

    fmt_set_mode(FMT_MODE_320x240_16BPP);

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
}
#endif

void start_main(void)
{
#if FMT_PIXEL_TEST_BPP == 15
    draw_15bpp_test();
#else
    draw_8bpp_test();
#endif
    while (1) {
        fmt_wait_vsync();
    }
}
