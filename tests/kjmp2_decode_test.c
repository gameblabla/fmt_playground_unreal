#include <stdint.h>
#include <stdio.h>
#include "kjmp2_fast.h"

int main(int argc, char **argv)
{
    uint8_t frame[305];
    kjmp2v_psg10_sample_t decoded[KJMP2_SAMPLES_PER_FRAME];
    kjmp2v_context_t decoder;
    FILE *in, *out;
    unsigned frame_size, i, frames = 0;

    if (argc != 3 || !(in = fopen(argv[1], "rb")) || !(out = fopen(argv[2], "wb"))) return 2;
    kjmp2v_init(&decoder);
    while (frames < 32 && fread(frame, 1, 4, in) == 4) {
        frame_size = 288u + ((frame[2] >> 1) & 1u);
        if (fread(frame + 4, 1, frame_size - 4, in) != frame_size - 4) return 3;
        if (kjmp2v_decode_frame_psg10(&decoder, frame, decoded) != frame_size) return 4;
        for (i = 0; i < KJMP2_SAMPLES_PER_FRAME; i++)
            fputc(KJMP2V_PSG10_LEFT(decoded[i]) >> 2, out);
        frames++;
    }
    return frames == 32 ? 0 : 5;
}
