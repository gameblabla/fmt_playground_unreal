/*
 * Decodes an MBV stream with the target's own decoder and checks it against
 * what the encoder believed it was producing.
 *
 * This is the test that makes MBV's in-place reconstruction safe to rely on.
 * The codec has no separate previous-frame buffer: SKIP leaves pixels alone
 * and MOTION copies within the buffer it is writing, so a vector can source
 * pixels the same frame has already changed.  That is only sound because the
 * encoder reconstructs through the identical primitives in the identical
 * order.  "Identical" is an assertion about two pieces of code, and this is
 * where it gets checked - on real encoded output, every frame, byte for byte,
 * palette included.  If the two ever drift, the picture on the machine
 * degrades over a whole GOP and nothing else in the build would notice.
 *
 * With a reference RGB24 file it also reports PSNR, which is not a pass/fail
 * criterion but is the number to watch when changing the encoder.
 *
 *   mbv_roundtrip_test stream.mbv recon.dump [source.rgb24]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "mbv.h"

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    fmt_mbv_info info;
    fmt_mbv_dec dec;
    uint8_t *stream, *dump, *ref = NULL, *frame;
    size_t slen, dlen, rlen = 0, pos;
    size_t px, dump_frame;
    uint32_t i, audio_total = 0, biggest = 0;
    double mse_sum = 0.0;
    int ref_frames = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: %s stream.mbv recon.dump [source.rgb24]\n", argv[0]);
        return 2;
    }
    stream = slurp(argv[1], &slen);
    dump   = slurp(argv[2], &dlen);
    if (argc > 3) {
        ref = slurp(argv[3], &rlen);
    }

    if (slen < MBV_HEADER_BYTES || fmt_mbv_parse_header(stream, &info)) {
        fprintf(stderr, "FAIL: not an MBV1 stream\n");
        return 1;
    }
    px = (size_t)info.width * info.height;
    dump_frame = 768 + px;
    if (dlen != dump_frame * info.frame_count) {
        fprintf(stderr, "FAIL: dump holds %zu bytes, expected %zu\n",
                dlen, dump_frame * info.frame_count);
        return 1;
    }

    frame = malloc(px);
    fmt_mbv_dec_init(&dec, &info, frame, info.width);

    printf("%ux%u, %.2f fps, %u frames, audio %u Hz\n",
           info.width, info.height, info.fps_q8 / 256.0,
           info.frame_count, info.audio_rate);

    pos = MBV_HEADER_BYTES;
    for (i = 0; i < info.frame_count; i++) {
        const uint8_t *audio = NULL;
        uint16_t alen = 0;
        uint32_t clen = fmt_mbv_chunk_bytes(stream + pos, (uint32_t)(slen - pos));

        if (!clen || pos + clen > slen) {
            fprintf(stderr, "FAIL: frame %u runs past the end of the file\n", i);
            return 1;
        }
        if (clen > info.max_chunk) {
            fprintf(stderr, "FAIL: frame %u is %u bytes, header claims max %u\n",
                    i, clen, info.max_chunk);
            return 1;
        }
        if (clen > biggest) {
            biggest = clen;
        }
        if (fmt_mbv_decode_chunk(&dec, stream + pos, clen, &audio, &alen)) {
            fprintf(stderr, "FAIL: frame %u did not decode\n", i);
            return 1;
        }
        audio_total += alen;
        pos += clen;

        if (memcmp(dec.pal, dump + (size_t)i * dump_frame, 768)) {
            fprintf(stderr, "FAIL: frame %u palette differs from the encoder's\n", i);
            return 1;
        }
        if (memcmp(frame, dump + (size_t)i * dump_frame + 768, px)) {
            size_t j;
            for (j = 0; j < px; j++) {
                if (frame[j] != dump[(size_t)i * dump_frame + 768 + j]) {
                    break;
                }
            }
            fprintf(stderr, "FAIL: frame %u differs from the encoder's "
                            "reconstruction, first at pixel %zu (%u,%u): "
                            "decoder %u, encoder %u\n",
                    i, j, (unsigned)(j % info.width), (unsigned)(j / info.width),
                    frame[j], dump[(size_t)i * dump_frame + 768 + j]);
            return 1;
        }

        if (ref && (size_t)(i + 1) * px * 3 <= rlen) {
            const uint8_t *src = ref + (size_t)i * px * 3;
            double mse = 0.0;
            size_t j;
            for (j = 0; j < px; j++) {
                const uint8_t *c = dec.pal + (size_t)frame[j] * 3;
                double dr = (double)c[0] - src[j * 3 + 0];
                double dg = (double)c[1] - src[j * 3 + 1];
                double db = (double)c[2] - src[j * 3 + 2];
                mse += dr * dr + dg * dg + db * db;
            }
            mse_sum += mse / (double)(px * 3);
            ref_frames++;
        }
    }

    if (pos != slen) {
        fprintf(stderr, "FAIL: %zu bytes left over after the last frame\n",
                slen - pos);
        return 1;
    }
    if (audio_total != info.audio_samples) {
        fprintf(stderr, "FAIL: chunks carry %u audio samples, header says %u\n",
                audio_total, info.audio_samples);
        return 1;
    }

    printf("%u frames decoded, every one identical to the encoder's "
           "reconstruction\n", info.frame_count);
    printf("largest chunk %u bytes, audio %u samples (%.2fs)\n",
           biggest, audio_total, (double)audio_total / info.audio_rate);
    if (ref_frames) {
        double mse = mse_sum / ref_frames;
        printf("PSNR vs source: %.2f dB over %d frames\n",
               mse > 0.0 ? 10.0 * log10(255.0 * 255.0 / mse) : 99.0, ref_frames);
    }
    return 0;
}
