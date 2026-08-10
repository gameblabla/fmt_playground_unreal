#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "mp2.h"
#ifdef FMT_MP2_DEBUG
extern unsigned fmt_mp2_debug_bits;
extern unsigned fmt_mp2_debug_q;
extern unsigned fmt_mp2_debug_calls;
extern unsigned fmt_mp2_debug_requested;
extern uint8_t fmt_mp2_debug_alloc[30];
extern uint8_t fmt_mp2_debug_raw[30];
#endif

int main(int argc, char **argv)
{
    uint8_t frame[305], pcm[FMT_MP2_SAMPLES_PER_FRAME];
    unsigned n, i, frames = 0, lo = 255, hi = 0;
    FILE *f, *out = 0;
    if ((argc != 2 && argc != 3) || !(f = fopen(argv[1], "rb"))) return 2;
    if (argc == 3 && !(out = fopen(argv[2], "wb"))) return 2;
    while (frames < 32 && fread(frame, 1, 4, f) == 4) {
        long next;
        if (!(n = fmt_mp2_frame_size(frame)) ||
            fread(frame + 4, 1, n - 4 + 16, f) != n - 4 + 16) return 3;
        next = ftell(f) - 16;
        i = (unsigned)fmt_mp2_decode_frame(frame, n + 16, pcm);
        if (i != 0) { printf("decode=%d bits=%u q=%u calls=%u requested=%u", (int)i,
#ifdef FMT_MP2_DEBUG
                          fmt_mp2_debug_bits, fmt_mp2_debug_q, fmt_mp2_debug_calls, fmt_mp2_debug_requested
#else
                          0u, 0u, 0u, 0u
#endif
                        );
#ifdef FMT_MP2_DEBUG
        for (i = 0; i < 30; i++) printf(" %u", fmt_mp2_debug_raw[i]);
        puts("");
        for (i = 0; i < 30; i++) printf(" %u", fmt_mp2_debug_alloc[i]);
#endif
        puts(""); return 3; }
        for (i = 0; i < FMT_MP2_SAMPLES_PER_FRAME; i++) {
            if (pcm[i] < lo) lo = pcm[i];
            if (pcm[i] > hi) hi = pcm[i];
        }
        if (out && fwrite(pcm, 1, FMT_MP2_SAMPLES_PER_FRAME, out) != FMT_MP2_SAMPLES_PER_FRAME)
            return 4;
        frames++;
        if (fseek(f, next, SEEK_SET)) return 3;
    }
    printf("frames=%u pcm=%u..%u\n", frames, lo, hi);
    return frames == 0 || lo == hi;
}
