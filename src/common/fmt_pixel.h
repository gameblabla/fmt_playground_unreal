#ifndef FMT_PIXEL_H
#define FMT_PIXEL_H

#include <stdint.h>

/*
 * Fast pixel-plot primitives for FM TOWNS *linear* addressing modes
 * only (fmt_mode_t.linear==1, i.e. 2-page/VRAM0Trans - see libfmt.h).
 * Each is named set_pixel_<width>_<bpp> and hardcodes that mode's
 * VRAM stride, so there's no runtime stride lookup/branch: just an
 * offset computation and one store.
 *
 * Do NOT call these against single-page (swizzled) modes such as
 * FMT_MODE_256x240_8BPP/FMT_MODE_320x240_8BPP/FMT_MODE_320x240_16BPP -
 * VRAM byte order there follows vram_singlepage_trans() (libfmt.c),
 * not row-major, so a direct offset write lands on the wrong pixel.
 * Use fmt_put_image() for those, or add a set_pixel that runs the
 * offset through the same transform if a single-page fast-path is
 * ever needed.
 *
 * We run with flat, 4GB-limit segments throughout - the real-mode ->
 * protected-mode transition (src/boot/setup.S, src/boot/head.S) loads
 * a big-limit descriptor once and never touches it again (the classic
 * "unreal mode" trick, just carried into full 32-bit protected mode
 * instead of staying in real mode). That means every segment register
 * already maps the full 32-bit physical address space identically, so
 * a plain physical address is a valid pointer under any of them: none
 * of the asm below reloads a segment register (no `mov %ax,%gs` etc.)
 * before touching VRAM - it would be redundant work on every single
 * pixel plot.
 */
#define FMT_VRAM0_LINEAR_BASE 0xA00000u

#define FMT_DEFINE_SET_PIXEL_8(NAME, WIDTH)                                  \
static inline void NAME(uint16_t x, uint16_t y, uint8_t color)               \
{                                                                             \
    volatile uint8_t *p =                                                    \
        (volatile uint8_t *)(FMT_VRAM0_LINEAR_BASE + (uint32_t)y * (WIDTH) + x); \
    __asm__ volatile (                                                       \
        "movb %1, (%0)"                                                      \
        :                                                                    \
        : "r" (p), "q" (color)                                               \
        : "memory"                                                           \
    );                                                                       \
}

#define FMT_DEFINE_SET_PIXEL_16(NAME, WIDTH)                                        \
static inline void NAME(uint16_t x, uint16_t y, uint16_t color)                     \
{                                                                                    \
    volatile uint16_t *p = (volatile uint16_t *)(FMT_VRAM0_LINEAR_BASE               \
        + (uint32_t)y * (WIDTH) * 2u + (uint32_t)x * 2u);                           \
    __asm__ volatile (                                                              \
        "movw %1, (%0)"                                                             \
        :                                                                           \
        : "r" (p), "r" (color)                                                      \
        : "memory"                                                                  \
    );                                                                              \
}

/* 8bpp: single-page-only bit depth (see libfmt.h), so these are only
 * valid to call if you've built a linear 8bpp mode yourself - none of
 * libfmt's built-in modes are both 8bpp and linear. Provided for
 * completeness/forward compatibility. */
FMT_DEFINE_SET_PIXEL_8(set_pixel_256_8, 256)
FMT_DEFINE_SET_PIXEL_8(set_pixel_320_8, 320)
FMT_DEFINE_SET_PIXEL_8(set_pixel_512_8, 512)
FMT_DEFINE_SET_PIXEL_8(set_pixel_640_8, 640)

/* 16bpp (RGB555 - build values with common.h's rgb15()): matches
 * FMT_MODE_320x240_16BPP (single-page - do not use these against it),
 * FMT_MODE_640x400_16BPP_LINEAR, FMT_MODE_512x480_16BPP_LINEAR. */
FMT_DEFINE_SET_PIXEL_16(set_pixel_320_16, 320)
FMT_DEFINE_SET_PIXEL_16(set_pixel_512_16, 512)
FMT_DEFINE_SET_PIXEL_16(set_pixel_640_16, 640)

#endif
