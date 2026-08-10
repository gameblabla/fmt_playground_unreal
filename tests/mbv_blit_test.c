/*
 * Checks the fast VRAM blit against libfmt's byte-at-a-time address transform.
 *
 * src/common/mbv_blit.h writes whole 32-bit words to two linear pointers
 * instead of computing libfmt.c's VRAM1Trans swizzle per byte, on the argument
 * that the swizzle turns the source's even and odd four-byte groups into two
 * dense arrays 256KB apart.  That argument is worth exactly as much as a test
 * of it: an off-by-one in the derivation would put a shredded picture on the
 * screen and there is no assertion on real hardware to catch it.
 *
 * So: blit a page of known bytes into a fake VRAM, then walk every source
 * offset and check the byte landed where libfmt would have put it.  Both
 * pages of the 256x240 8bpp mode are covered, since the second page's base
 * offset is what exercises the `base >> 1` term.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "mbv_blit.h"

#define VRAM_SIZE   0x80000u
#define PAGE_BYTES  (256u * 240u)

/* libfmt.c's vram_singlepage_trans(), verbatim - the reference. */
static uint32_t vram_singlepage_trans(uint32_t off)
{
    return ((off & 4u) << 16) | ((off & 0x7fff8u) >> 1) | (off & 3u);
}

int main(void)
{
    uint8_t *vram = calloc(1, VRAM_SIZE);
    uint8_t *ref  = calloc(1, VRAM_SIZE);
    uint8_t *src  = malloc(PAGE_BYTES);
    uint32_t page, off, i;
    int fails = 0;

    if (!vram || !ref || !src) {
        return 2;
    }

    for (page = 0; page < 2; page++) {
        uint32_t base = page * PAGE_BYTES;

        /* A pattern where every byte of the page is distinguishable from its
         * neighbours in all three low bits the swizzle cares about. */
        for (i = 0; i < PAGE_BYTES; i++) {
            src[i] = (uint8_t)(i * 7u + page * 131u);
        }

        memset(vram, 0, VRAM_SIZE);
        memset(ref, 0, VRAM_SIZE);
        fmt_mbv_blit(vram, base, src, PAGE_BYTES);
        for (i = 0; i < PAGE_BYTES; i++) {
            ref[vram_singlepage_trans(base + i)] = src[i];
        }

        for (off = 0; off < VRAM_SIZE; off++) {
            if (vram[off] != ref[off]) {
                if (fails < 8) {
                    printf("FAIL: page %u, VRAM offset 0x%05x: blit wrote %u, "
                           "libfmt's transform wants %u\n",
                           page, off, vram[off], ref[off]);
                }
                fails++;
            }
        }
        printf("page %u (base 0x%05x): %u bytes, %s\n", page, base, PAGE_BYTES,
               fails ? "MISMATCH" : "every byte where libfmt would put it");
    }

    /* The blit must also stay inside the page it was given: nothing outside
     * the two banks' 30720-byte spans may have been touched. */
    if (fails) {
        printf("%d mismatching bytes\n", fails);
        return 1;
    }
    return 0;
}
