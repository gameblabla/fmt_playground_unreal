/* Decodes a whole MP2 asset through kjmp2v_decode_frame_pcm8() and writes the
 * raw unsigned 8-bit result.  Built twice by `make test` - once with the i386
 * window kernel and once with FMT_MP2_NO_ASM - so the two outputs can be
 * compared byte for byte.  The assembly is only worth having if it is exactly
 * the C it replaced. */
#include <stdint.h>
#include <stdio.h>
#include "kjmp2_fast.h"

int main(int argc, char **argv)
{
    static uint8_t in[8u << 20];
    static uint8_t pcm[KJMP2_SAMPLES_PER_FRAME];
    kjmp2v_context_t dec;
    FILE *f, *out;
    size_t n, pos = 0;
    unsigned frames = 0;

    if (argc != 3) { fprintf(stderr, "usage: %s in.mp2 out.u8\n", argv[0]); return 2; }
    if (!(f = fopen(argv[1], "rb"))) return 2;
    n = fread(in, 1, sizeof in, f);
    fclose(f);
    if (!(out = fopen(argv[2], "wb"))) return 2;

    kjmp2v_init(&dec);
    while (pos + 320 <= n) {
        unsigned long size = kjmp2v_decode_frame_pcm8(&dec, in + pos, pcm);
        if (!size) { fprintf(stderr, "frame %u rejected at offset %lu\n",
                             frames, (unsigned long)pos); return 3; }
        if (fwrite(pcm, 1, KJMP2_SAMPLES_PER_FRAME, out) != KJMP2_SAMPLES_PER_FRAME) return 4;
        pos += size;
        frames++;
    }
    fclose(out);
    fprintf(stderr, "%u frames, %u samples, %.2fs of audio\n",
            frames, frames * KJMP2_SAMPLES_PER_FRAME, frames * 1152.0 / 16000.0);
    return frames == 0;
}
