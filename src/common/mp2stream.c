#include "mp2stream.h"
#include "mp2.h"
#include "kjmp2_fast.h"
#include "io.h"
#include "cdrom.h"
#include "iso9660.h"

#define YM_ADDR0 0x4D8
#define YM_DATA0 0x4DA
#define YM_REG_DAC_ENABLE 0x2B
#define YM_REG_DAC_DATA 0x2A
#define YM_BUSY 0x80
#define YM_WAIT_1US 0x6C
#define YM_SOUND_MUTE 0x4D5
#define YM_SOUND_AUDIO 0x4EC

/* 32 KiB compressed input is eight seconds at 32 kbit/s.  The decoded
 * ring is one half-second at 16 kHz.  Keeping these separate is what lets
 * the CDC run at sector granularity while the DAC is serviced per sample. */
#define MP2_INPUT_RING 32768u
#define MP2_PCM_RING 8192u
#define MP2_LOOKAHEAD 16u

static uint8_t g_input[MP2_INPUT_RING];
static uint8_t g_pcm[MP2_PCM_RING];
static uint8_t g_frame[289 + MP2_LOOKAHEAD];
static kjmp2v_psg10_sample_t g_decoded_psg[FMT_MP2_SAMPLES_PER_FRAME];
static kjmp2v_context_t g_decoder;
static uint32_t g_lba, g_size;
static uint8_t g_loaded;
static volatile uint8_t g_stop;

static void ym_write(uint8_t reg, uint8_t val)
{
    outb(reg, YM_ADDR0);
    outb(val, YM_DATA0);
}

int fmt_mp2_stream_load_file(const char *name)
{
    g_loaded = 0;
    if (fmt_iso9660_find(name, &g_lba, &g_size) || !g_size) return -1;
    kjmp2v_init(&g_decoder);
    g_loaded = 1;
    return 0;
}

void fmt_mp2_stream_stop(void)
{
    g_stop = 1;
    outb(0, YM_SOUND_MUTE);
}

/* Decode as many complete frames as currently fit.  `input_pos` is the
 * CDC ring consumer cursor, and `pcm_write` is in 16 kHz samples. */
static int decode_ready(fmt_cd_stream *cd, uint32_t *input_pos,
                        uint32_t pcm_read, uint32_t *pcm_write,
                        unsigned wanted)
{
    while (*pcm_write - pcm_read < wanted) {
        unsigned size, i;
        if (cd->fill_pos - *input_pos < 4) return 0;
        for (i = 0; i < 4; i++) g_frame[i] = g_input[(*input_pos + i) & (MP2_INPUT_RING - 1u)];
        size = fmt_mp2_frame_size(g_frame);
        if (!size) return -1; /* generated asset has no tags/ancillary frames */
        if (cd->fill_pos - *input_pos < size + MP2_LOOKAHEAD) return 0;
        if (MP2_PCM_RING - (*pcm_write - pcm_read) < FMT_MP2_SAMPLES_PER_FRAME) return 0;
        for (i = 4; i < size + MP2_LOOKAHEAD; i++)
            g_frame[i] = g_input[(*input_pos + i) & (MP2_INPUT_RING - 1u)];
        if (kjmp2v_decode_frame_psg10(&g_decoder, g_frame, g_decoded_psg) != size) return -1;
        for (i = 0; i < FMT_MP2_SAMPLES_PER_FRAME; i++)
            g_pcm[(*pcm_write + i) & (MP2_PCM_RING - 1u)] =
                (uint8_t)(KJMP2V_PSG10_LEFT(g_decoded_psg[i]) >> 2);
        *input_pos += size;
        *pcm_write += FMT_MP2_SAMPLES_PER_FRAME;
    }
    return 0;
}

int fmt_mp2_stream_play_streaming(void)
{
    fmt_cd_stream cd;
    uint32_t input_pos = 0, pcm_read = 0, pcm_write = 0;
    uint8_t repeat = 0;

    if (!g_loaded) return -1;
    g_stop = 0;
    fmt_cdrom_stream_init(&cd, g_lba, g_size, g_input, MP2_INPUT_RING, 1);

    /* Prime a quarter-second of decoded samples before enabling DAC output. */
    while (pcm_write - pcm_read < 4096u) {
        if (cd.state == FMT_CD_STREAM_ERROR || decode_ready(&cd, &input_pos, pcm_read, &pcm_write, 4096u)) return -1;
        fmt_cdrom_stream_step(&cd, input_pos, 256);
    }

    outb(0x03, YM_SOUND_MUTE);
    outb(0x7f, YM_SOUND_AUDIO);
    ym_write(YM_REG_DAC_ENABLE, 0x80);
    while (inb(YM_ADDR0) & YM_BUSY) {}
    outb(YM_REG_DAC_DATA, YM_ADDR0);
    __asm__ __volatile__("cli");

    for (;;) {
        if (g_stop) break;
        outb(0, YM_WAIT_1US);
        outb(g_pcm[pcm_read & (MP2_PCM_RING - 1u)], YM_DATA0);
        if (repeat) pcm_read++;
        repeat ^= 1; /* 16 kHz MP2 -> DAC's approximately 32 kHz pace. */

        while (inb(YM_ADDR0) & YM_BUSY) {
            fmt_cdrom_stream_step(&cd, input_pos, 1);
            if (g_stop) goto done;
        }
        /* This is deliberately outside the busy window: a whole Layer-II
         * frame is too much work to hide in one 30us interval.  The 4 KiB
         * decoded reserve absorbs its short burst on a 386SX. */
        if (pcm_write - pcm_read < 4096u &&
            decode_ready(&cd, &input_pos, pcm_read, &pcm_write, 6144u)) goto done;
        if (cd.state == FMT_CD_STREAM_ERROR) goto done;
    }
done:
    ym_write(YM_REG_DAC_ENABLE, 0);
    return 0;
}
