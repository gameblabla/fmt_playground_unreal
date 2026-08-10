#include "mbvplay.h"
#include "mbv.h"
#include "mbv_blit.h"
#include "dacout.h"
#include "libfmt.h"
#include "palette.h"
#include "io.h"
#include "cdrom.h"
#include "iso9660.h"

/* See mbvplay.h for how the four jobs in here share the CPU. */

/* The disc is streamed into this ring.  Power of two, a multiple of 2048, and
 * comfortably larger than the largest chunk in any stream the encoder will
 * produce (tools/mbvenc.c's -m ceiling, 30000 by default): a whole chunk has
 * to be resident before it can be decoded, and the drive has to be able to
 * keep working ahead of that while it is. */
#define MBV_RING        65536u

/* Chunks are decoded in place out of the ring; this is only used for the one
 * chunk in every ring-full that straddles the wrap. */
#define MBV_SCRATCH     32768u

/* 256x240 8bpp - the only mode this player targets.  Sized statically because
 * there is no allocator here and no reason to want one. */
#define MBV_MAX_W       256u
#define MBV_MAX_H       240u

/* One frame of audio at 12fps/16kHz is 1334 bytes; leave room for slower
 * frame rates without making the encoder's job a special case. */
#define MBV_AUDIO_MAX   4096u

static uint8_t g_ring[MBV_RING];
static uint8_t g_scratch[MBV_SCRATCH];
static uint8_t g_frame[MBV_MAX_W * MBV_MAX_H];
static uint8_t g_audio[2][MBV_AUDIO_MAX];

/* Diagnostic counters, published to a fixed physical address so playback can
 * be watched in-machine with `MEMDUMP PHYS:00100000 28 1` in the emulator's
 * console while it runs.  Enabled by `make VIDEO_PLAYER=1 MBV_STATS=1`.
 *
 * Not 0x00080000, which is where mp2stream.c and the YM busy probe publish
 * theirs: this player's buffers are ~160KB of .bss and the payload loads at
 * 0x10000, so 0x80000 is inside them.
 *
 * Layout, little-endian 32-bit after the magic:
 *   0  "MBVS"
 *   4  stage reached (see MBV_STAGE_* below)
 *   8  frames presented
 *  12  bytes consumed from the stream
 *  16  DAC underruns
 *  20  streaming reader state (FMT_CD_STREAM_*)
 *  24  0xA5A5A5A5 trailer
 */
#ifdef FMT_MBV_STATS
#define MBV_STATS ((volatile uint32_t *)0x00100000u)
#define MBV_STAGE_LOADED    1u
#define MBV_STAGE_MODE      2u
#define MBV_STAGE_PRIMED    3u
#define MBV_STAGE_FIRST     4u
#define MBV_STAGE_PLAYING   5u
#define MBV_STAGE_DONE      6u
static uint32_t g_stat_stage, g_stat_frames, g_stat_pos;
static void mbv_stats(const fmt_cd_stream *cd)
{
    MBV_STATS[0] = 0x5356424du;    /* "MBVS" */
    MBV_STATS[1] = g_stat_stage;
    MBV_STATS[2] = g_stat_frames;
    MBV_STATS[3] = g_stat_pos;
    MBV_STATS[4] = fmt_dac.underruns;
    MBV_STATS[5] = cd ? cd->state : 0xffffffffu;
    MBV_STATS[6] = 0xA5A5A5A5u;
}
#define MBV_STAGE(s, cd)  do { g_stat_stage = (s); mbv_stats(cd); } while (0)
#define MBV_PROGRESS(f, p, cd) \
    do { g_stat_frames = (f); g_stat_pos = (p); mbv_stats(cd); } while (0)
#else
#define MBV_STAGE(s, cd)        ((void)0)
#define MBV_PROGRESS(f, p, cd)  ((void)0)
#endif

static fmt_mbv_info g_info;
static fmt_mbv_dec  g_dec;
static uint32_t g_lba, g_size;
static uint8_t  g_loaded;
static volatile uint8_t g_stop;

/* fmt_flip_page_poll() takes a plain function pointer, and this is all it
 * needs to do: keep the sample clock running across the vertical blank. */
static void poll_dac(void)
{
    fmt_dac_tick();
}

int fmt_mbv_stream_load_file(const char *name)
{
    g_loaded = 0;
    if (fmt_iso9660_find(name, &g_lba, &g_size) != 0 || g_size <= MBV_HEADER_BYTES) {
        return -1;
    }
    /* The header is the first 32 bytes of the file; one sector read is the
     * cheapest way to see them, and the streaming reader will fetch the
     * sector again from the start when playback begins. */
    if (fmt_cdrom_read(g_lba, 1, g_scratch) != 0) {
        return -1;
    }
    if (fmt_mbv_parse_header(g_scratch, &g_info) != 0) {
        return -1;
    }
    if (g_info.width > MBV_MAX_W || g_info.height > MBV_MAX_H ||
        g_info.max_chunk > MBV_SCRATCH || g_info.max_chunk == 0) {
        return -1;
    }
    g_loaded = 1;
    MBV_STAGE(MBV_STAGE_LOADED, (fmt_cd_stream *)0);
    return 0;
}

void fmt_mbv_stream_stop(void)
{
    g_stop = 1;
}

uint32_t fmt_mbv_stream_underruns(void)
{
    return fmt_dac.underruns;
}

/*
 * Locates the whole chunk at `pos`.  Returns 1 with `out`/`out_len` set, 0 if
 * the drive has not delivered all of it yet, or -1 if the stream is corrupt.
 *
 * The common case hands back a pointer straight into the ring and copies
 * nothing: the streaming reader never overwrites bytes ahead of the read
 * cursor we pass it, and the cursor stays parked at the start of this chunk
 * until it has been decoded.  Only a chunk that straddles the ring's wrap has
 * to be linearised into g_scratch, which is one chunk in every 64KB.
 */
static int chunk_at(fmt_cd_stream *cd, uint32_t pos,
                    const uint8_t **out, uint32_t *out_len)
{
    uint8_t hdr[MBV_CHUNK_HEADER_BYTES];
    uint32_t avail = cd->fill_pos - pos;
    uint32_t start = pos & (MBV_RING - 1u);
    uint32_t clen, i;

    if (avail < MBV_CHUNK_HEADER_BYTES) {
        return 0;
    }
    for (i = 0; i < MBV_CHUNK_HEADER_BYTES; i++) {
        hdr[i] = g_ring[(pos + i) & (MBV_RING - 1u)];
    }
    clen = fmt_mbv_chunk_bytes(hdr, MBV_CHUNK_HEADER_BYTES);
    if (clen == 0 || clen > MBV_SCRATCH) {
        return -1;            /* corrupt stream, or one this build cannot hold */
    }
    if (avail < clen) {
        return 0;
    }

    *out_len = clen;
    if (start + clen <= MBV_RING) {
        *out = g_ring + start;
        return 1;
    }
    for (i = 0; i < clen; i++) {
        g_scratch[i] = g_ring[(pos + i) & (MBV_RING - 1u)];
    }
    *out = g_scratch;
    return 1;
}

static void upload_palette(void)
{
    unsigned i;

    for (i = 0; i < 256u; i++) {
        set_palette((uint8_t)i, g_dec.pal[i * 3], g_dec.pal[i * 3 + 1],
                    g_dec.pal[i * 3 + 2]);
        fmt_dac_tick();
    }
}

/* Blit the decoded frame into the page being drawn, show it, then apply any
 * new palette.  Order matters: a keyframe's palette belongs to the picture
 * that arrives with it, and loading it before the flip would apply it to the
 * frame still on screen for one frame's worth of wrong colours. */
static void present(void)
{
    fmt_mbv_blit((volatile uint8_t *)FMT_VRAM0_BASE, g_fmt_draw_buffer_offset,
                 g_frame, (uint32_t)g_info.width * g_info.height);
    fmt_flip_page_poll(poll_dac);
    if (g_dec.pal_dirty) {
        upload_palette();
    }
}

/* Audio has to be copied out of the ring: the samples belong to a chunk whose
 * bytes the drive is free to overwrite as soon as the read cursor moves past
 * them, and they are still playing while the next chunk is being fetched. */
static uint16_t take_audio(const uint8_t *audio, uint16_t alen, uint8_t *dst)
{
    uint16_t i;

    if (alen > MBV_AUDIO_MAX) {
        alen = MBV_AUDIO_MAX;
    }
    for (i = 0; i < alen; i++) {
        dst[i] = audio[i];
        if ((i & 63u) == 0u) {
            fmt_dac_tick();
        }
    }
    return alen;
}

int fmt_mbv_stream_play(void)
{
    fmt_cd_stream cd;
    uint32_t pos = MBV_HEADER_BYTES;
    uint32_t clen = 0;
    uint32_t frame;
    const uint8_t *chunk, *audio;
    uint16_t alen;
    uint8_t which = 0;

    if (!g_loaded) {
        return -1;
    }
    g_stop = 0;

    fmt_set_mode(FMT_MODE_256x240_8BPP);
    if (!fmt_page_flipping_available()) {
        return -1;
    }
    fmt_mbv_dec_init(&g_dec, &g_info, g_frame, g_info.width);
    MBV_STAGE(MBV_STAGE_MODE, (fmt_cd_stream *)0);

    fmt_cdrom_stream_init(&cd, g_lba, g_size, g_ring, MBV_RING, 0);

    /* Fill the ring before making a sound.  Nothing is playing yet, so this
     * may take as long as the drive wants. */
    while (cd.fill_pos < MBV_RING) {
        if (cd.state == FMT_CD_STREAM_ERROR) {
            return -1;
        }
        if (cd.state == FMT_CD_STREAM_DONE) {
            break;   /* whole file is shorter than the ring */
        }
        fmt_cdrom_stream_step(&cd, 0, 64);
    }

    MBV_STAGE(MBV_STAGE_PRIMED, &cd);

    /* Frame 0: a keyframe, so it brings the palette with it. */
    for (;;) {
        int r = chunk_at(&cd, pos, &chunk, &clen);
        if (r > 0) {
            break;
        }
        if (r < 0 || cd.state == FMT_CD_STREAM_ERROR ||
            cd.state == FMT_CD_STREAM_DONE) {
            return -1;
        }
        fmt_cdrom_stream_step(&cd, pos, 256);
    }
    if (fmt_mbv_decode_chunk(&g_dec, chunk, clen, &audio, &alen) != 0) {
        return -1;
    }
    alen = take_audio(audio, alen, g_audio[0]);
    pos += clen;

    MBV_STAGE(MBV_STAGE_FIRST, &cd);
    present();

    fmt_dac_start();
    fmt_dac_submit(g_audio[0], alen);

    /* Interrupts stay masked for the same reason pcmstream.c masks them: an
     * IRQ landing between two DAC writes steals time the pacing assumes it
     * has, and this payload's IDT has no hardware vectors anyway. */
    __asm__ __volatile__("cli");

    MBV_STAGE(MBV_STAGE_PLAYING, &cd);

    for (frame = 1; frame < g_info.frame_count; frame++) {
        uint8_t next = which ^ 1u;

        /* Wait for the whole of the next chunk, ticking the DAC and stepping
         * the drive one I/O operation at a time while it arrives. */
        for (;;) {
            int r;
            if (g_stop) {
                goto done;
            }
            r = chunk_at(&cd, pos, &chunk, &clen);
            if (r > 0) {
                break;
            }
            if (r < 0 || cd.state == FMT_CD_STREAM_ERROR ||
                cd.state == FMT_CD_STREAM_DONE) {
                goto done;
            }
            fmt_dac_tick();
            fmt_cdrom_stream_step(&cd, pos, 1);
        }

        /* Decode into the persistent frame buffer.  The decoder ticks the DAC
         * once per macroblock, so the current frame's audio keeps playing
         * throughout. */
        if (fmt_mbv_decode_chunk(&g_dec, chunk, clen, &audio, &alen) != 0) {
            goto done;
        }
        alen = take_audio(audio, alen, g_audio[next]);
        pos += clen;

        /* Hand over only once the frame on screen has played out its audio.
         * This is what keeps sound and picture together. */
        while (!fmt_dac_drained()) {
            if (g_stop) {
                goto done;
            }
            fmt_dac_tick();
            fmt_cdrom_stream_step(&cd, pos, 1);
        }
        fmt_dac_submit(g_audio[next], alen);
        which = next;

        /* The new audio is now playing, which is exactly the cover the blit
         * and the wait for vertical blank need. */
        present();
        MBV_PROGRESS(frame, pos, &cd);
    }

done:
    MBV_STAGE(MBV_STAGE_DONE, &cd);
    fmt_dac_stop();
    return 0;
}
