#ifndef LIBFMT_H
#define LIBFMT_H

#include <stdint.h>
#include "common.h"

/*
 * libfmt: small helper library for setting up FM TOWNS video modes
 * (CRTC + palette bank + VRAM addressing) and blitting into them.
 *
 * Two VRAM addressing schemes exist on the FM TOWNS low-res CRTC,
 * selected by video[0] (sifter[0]) bit 0x10 (TownsCRTC::
 * LowResCrtcIsInSinglePageMode(), TOWNSEMU/src/towns/crtc/crtc.cpp):
 *
 *   - Single-page mode (bit clear): the renderer reads VRAM through
 *     the VRAM1Trans byte-address swizzle (see fmt_put_image() below).
 *     A plain row-major write disagrees with what's displayed.
 *   - 2-page ("linear") mode (bit set): the renderer reads VRAM
 *     through VRAM0Trans, which TOWNSEMU/src/towns/render/render.h
 *     defines as a no-op - i.e. genuinely linear/row-major addressing.
 *     This is what the fast set_pixel_*() routines in fmt_pixel.h are
 *     written for.
 *
 * fmt_mode_t.linear records which scheme a given mode's video[] byte
 * selects, so fmt_put_image() can pick the matching write path.
 *
 * The two schemes also disagree on page bit depth (TownsCRTC::
 * GetPageBitsPerPixel) and bytes-per-line (TownsCRTC::
 * GetPageBytesPerLine, both in crtc.cpp):
 *   - Single-page: CR0 CL bits 2->16bpp, 3->8bpp; bytesPerLine = LO0*8.
 *   - 2-page/linear: CR0 CL bits 1->16bpp, 3->4bpp (there is no 8bpp
 *     encoding in 2-page mode); bytesPerLine = LO0*4.
 * So true 8bpp only exists in single-page mode - the 2-page/linear
 * modes below are 16bpp (RGB555, see common.h's rgb15()).
 *
 * 15kHz modes (256x240/320x240) share one geometry/timing
 * (HSW/HST/VST/EET/EHAJ/EVAJ), carried forward from the
 * verified-working 256x240 8bpp register set (see git history b2020fc
 * and earlier), differing only in width/bpp (HDE0-HDS0, LO0) or
 * vertical size (VDE0-VDS0).
 *
 * 640x400/512x480 run at 31kHz, with a different HST (CRTC total
 * horizontal dot count) and clock select (CR1 bits[1:0]) - derived
 * from TownsCRTC::GetHorizontalFrequency()'s formula
 * (CLKSELtoHz[CLKSEL]/HST)/1000 == 31, using CLKSEL=2 (25175000Hz,
 * the same 25.175MHz dot clock as VGA 640x480@60Hz) and HST=800
 * (0x320) -> 25175000/800/1000 == 31 exactly. At this frequency,
 * TownsCRTC::GetPageSizeOnMonitor() maps monitor size directly to
 * HDE0-HDS0 / VDE0-VDS0 (no /2 or *2 fixups - those only apply at
 * 15kHz), which is much simpler to reason about. Both 31kHz modes run
 * in 2-page/linear 16bpp mode. Verified booting cleanly in TOWNSEMU
 * (see commit history) - not validated against real Marty hardware.
 */
typedef enum {
    FMT_MODE_256x240_8BPP,
    FMT_MODE_320x240_8BPP,
    FMT_MODE_320x240_16BPP,
    FMT_MODE_640x400_16BPP_LINEAR,
    FMT_MODE_512x480_16BPP_LINEAR,
    FMT_NUM_MODES
} fmt_mode_id_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;        /* 8 or 16 */
    uint16_t stride;      /* bytes per VRAM line */
    uint8_t  linear;      /* 1 = 2-page/VRAM0Trans (row-major), 0 = single-page/VRAM1Trans (swizzled) */
    crtc_set_t crtc;
    video_set_t video;
} fmt_mode_t;

/* Stops the display, programs CRTC + palette-bank registers for the
 * given mode, and restarts the display. Does NOT touch the palette
 * itself or VRAM contents - call fmt_load_palette()/fmt_put_image()
 * (or your own) after. */
void fmt_set_mode(fmt_mode_id_t id);

/* Mode last passed to fmt_set_mode(), or NULL if none set yet. */
const fmt_mode_t *fmt_current_mode(void);

/* Loads `count` RGB888 triplets (24-bit -> the analog palette DACs)
 * starting at palette index 0. */
void fmt_load_palette(const uint8_t *rgb888, int count);

/* Blits an 8bpp or 16bpp (per the current mode's bpp) linear image of
 * `width`x`height`, `stride` bytes/line, into VRAM at the current
 * mode's addressing (single-page: VRAM1Trans byte-address swizzle;
 * 2-page/linear: plain row-major - see libfmt.c). Must be called
 * after fmt_set_mode(). */
void fmt_put_image(const void *src, int width, int height, int stride);

void fmt_wait_vsync(void);

#endif
