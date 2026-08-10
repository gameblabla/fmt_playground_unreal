/*
 * Host test for the IC card reader (src/common/icm.c).
 *
 * The interesting part of that file is not reading memory - it is what
 * happens at a bank boundary. On a 386SX/Marty only 1MB of the card is
 * visible at a time, so every read of a multi-megabyte asset is really a
 * sequence of reads with a bank register write in between, and getting
 * that wrong produces a stream that is plausible for a while and then
 * quietly wrong - which on a real machine looks like a player that runs
 * for a few seconds and then sits there decoding nothing.
 *
 * The other thing under test is subtler, and is what actually broke: the
 * window must be left showing bank 0 whenever a read returns, because
 * the payload's own .rodata is linked into bank 0 (src/boot/icm.lds.S).
 * Leave another bank selected and the program's own constants are not
 * there any more.
 *
 * So the model here is the machine's: a real 1MB window mapped at the
 * real window address - so icm.c's own address arithmetic is what is
 * under test, not a substitute for it - and a bank register that pages a
 * megabyte of the model card into it, through the hook in
 * tests/hostio/io.h. Paging is a remap rather than a copy because the
 * reader switches banks twice per bite while streaming.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "icm.h"

#define CARD_MB     4u
#define CARD_BYTES  (CARD_MB * 1024u * 1024u)

static unsigned char *g_card;      /* the model card, all of it */
static int g_card_fd = -1;
static unsigned char *g_window;    /* what the CPU can see of it */
static unsigned long g_bank_writes;
static unsigned int g_cur_bank;

unsigned long host_now_us;
unsigned long host_dac_writes;
unsigned char host_dac_last;
unsigned long host_dac_times[1];
unsigned long host_dac_count;

void host_icm_bank_write(unsigned char bank)
{
    void *at;

    g_bank_writes++;
    g_cur_bank = bank;
    if ((unsigned)bank * FMT_ICM_WINDOW >= CARD_BYTES) {
        at = mmap(g_window, FMT_ICM_WINDOW, PROT_READ,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    } else {
        at = mmap(g_window, FMT_ICM_WINDOW, PROT_READ, MAP_SHARED | MAP_FIXED,
                  g_card_fd, (unsigned)bank * FMT_ICM_WINDOW);
    }
    if (at != (void *)g_window) {
        fprintf(stderr, "could not page bank %u into the window\n", bank);
        exit(1);
    }
}

/* Card contents no run of bytes repeats anywhere else in, so a read that
 * lands in the wrong bank cannot accidentally look right. */
static unsigned char model(unsigned int off)
{
    unsigned int x = off * 2654435761u;
    return (unsigned char)((x >> 13) ^ off);
}

static void put32(unsigned char *p, unsigned int v)
{
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

static int g_failures;

static void check(const char *what, int ok)
{
    printf("  %-58s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) {
        g_failures++;
    }
}

static unsigned char g_buf[0x140000 + 8];

static int read_matches(unsigned int off, unsigned int len, int dst_skew)
{
    unsigned int i;

    if (fmt_icm_read(off, len, g_buf + dst_skew) != 0) {
        return 0;
    }
    if (g_cur_bank != 0) {
        printf("    left bank %u selected after the read\n", g_cur_bank);
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (g_buf[dst_skew + i] != model(off + i)) {
            printf("    mismatch at +%u (card 0x%X): got %02X want %02X\n",
                   i, off + i, g_buf[dst_skew + i], model(off + i));
            return 0;
        }
    }
    return 1;
}

/* The offsets the card image that first showed the bug actually used: an
 * asset that starts near the end of bank 0 and runs on into bank 1. */
#define ASSET_OFF   0x000F0D90u
#define ASSET_SIZE  0x00030000u

int main(void)
{
    unsigned int i;
    unsigned char *toc;

    setvbuf(stdout, NULL, _IONBF, 0);

    g_card_fd = memfd_create("icmcard", 0);
    if (g_card_fd < 0 || ftruncate(g_card_fd, CARD_BYTES) != 0) {
        fprintf(stderr, "could not make the model card\n");
        return 77;
    }
    g_card = mmap(NULL, CARD_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, g_card_fd, 0);
    if (g_card == MAP_FAILED) {
        fprintf(stderr, "could not map the model card\n");
        return 77;
    }
    for (i = 0; i < CARD_BYTES; i++) {
        g_card[i] = model(i);
    }

    /* Header, as tools/mkicm.py writes it: the reader takes the card's
     * layout from the IPL sector sitting at offset 0. */
    memcpy(g_card, "IPL4", 4);
    put32(g_card + FMT_ICM_HDR_CARD_BASE, FMT_ICM_CARD_BASE);
    put32(g_card + FMT_ICM_HDR_CARD_BYTES, CARD_BYTES);
    put32(g_card + FMT_ICM_HDR_TOC_OFF, 0x800);

    toc = g_card + 0x800;
    put32(toc, FMT_ICM_TOC_MAGIC);
    put32(toc + 4, 1);
    memset(toc + 8, 0, FMT_ICM_TOC_NAME_LEN);
    memcpy(toc + 8, "MUSIC.MP2", 9);
    put32(toc + 8 + FMT_ICM_TOC_NAME_LEN, ASSET_OFF);
    put32(toc + 8 + FMT_ICM_TOC_NAME_LEN + 4, ASSET_SIZE);

    g_window = mmap((void *)FMT_ICM_CARD_BASE, FMT_ICM_WINDOW, PROT_READ,
                    MAP_SHARED | MAP_FIXED_NOREPLACE, g_card_fd, 0);
    if (g_window != (void *)FMT_ICM_CARD_BASE) {
        fprintf(stderr, "could not map a window at 0x%08X\n",
                (unsigned)FMT_ICM_CARD_BASE);
        return 77;
    }

    printf("IC card reader, %u MiB card through a %u KiB window at 0x%08X\n",
           CARD_MB, (unsigned)(FMT_ICM_WINDOW / 1024), (unsigned)FMT_ICM_CARD_BASE);

    check("fmt_icm_init() accepts the card", fmt_icm_init() == 0);

    {
        uint32_t off = 0, size = 0;
        check("fmt_icm_find() finds the asset",
              fmt_icm_find("MUSIC.MP2", &off, &size) == 0 &&
              off == ASSET_OFF && size == ASSET_SIZE);
        check("fmt_icm_find() is case-insensitive",
              fmt_icm_find("music.mp2", &off, &size) == 0);
        check("fmt_icm_find() rejects a name that is not there",
              fmt_icm_find("NOSUCH.BIN", &off, &size) != 0);
    }

    check("read wholly inside bank 0", read_matches(0x1000, 4096, 0));
    check("read wholly inside bank 2", read_matches(0x200000, 4096, 0));
    check("read ending exactly at the bank boundary",
          read_matches(FMT_ICM_WINDOW - 4096, 4096, 0));
    check("read starting exactly at the bank boundary",
          read_matches(FMT_ICM_WINDOW, 4096, 0));
    check("read straddling the boundary, all ends aligned",
          read_matches(FMT_ICM_WINDOW - 64, 128, 0));
    check("read straddling the boundary, source unaligned",
          read_matches(FMT_ICM_WINDOW - 65, 130, 0));
    check("read straddling the boundary, destination unaligned",
          read_matches(FMT_ICM_WINDOW - 64, 128, 1));
    check("read straddling the boundary, both unaligned",
          read_matches(FMT_ICM_WINDOW - 3, 7, 3));
    check("read spanning a whole bank", read_matches(0x0F0000, 0x120000, 0));
    check("read of the last byte of the card",
          read_matches(CARD_BYTES - 1, 1, 0));
    check("read running off the end of the card is refused",
          fmt_icm_read(CARD_BYTES - 4, 8, g_buf) != 0);
    check("the reader did have to switch banks", g_bank_writes > 1);

    /* And the streaming path, which is what the players actually use:
     * every byte of the asset, in the bites a sample-paced loop hands
     * out, has to come back in order. */
    {
        static unsigned char ring[32768];
        fmt_icm_stream st;
        uint32_t play_pos = 0;
        unsigned long idle = 0;
        int ok = 1;

        fmt_icm_stream_init(&st, ASSET_OFF, ASSET_SIZE, ring, sizeof ring, 0);
        while (play_pos < ASSET_SIZE) {
            uint32_t avail = st.fill_pos - play_pos;
            uint32_t take = avail < FMT_ICM_STEP_BYTES ? avail : FMT_ICM_STEP_BYTES;
            uint32_t j;

            if (!take) {
                if (st.state == FMT_ICM_STREAM_ERROR) {
                    printf("    stream reported an error at %u\n", play_pos);
                    ok = 0;
                    break;
                }
                if (st.state == FMT_ICM_STREAM_DONE) {
                    printf("    stream ended early at %u of %u\n", play_pos, ASSET_SIZE);
                    ok = 0;
                    break;
                }
                if (++idle > 100000UL) {
                    printf("    stream stopped delivering at %u\n", play_pos);
                    ok = 0;
                    break;
                }
            }
            for (j = 0; j < take; j++, play_pos++) {
                if (ring[play_pos & (sizeof ring - 1)] != model(ASSET_OFF + play_pos)) {
                    printf("    stream mismatch at %u (card 0x%X)\n",
                           play_pos, ASSET_OFF + play_pos);
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                break;
            }
            /* Refill exactly as a player's idle loop does: one bite. */
            fmt_icm_stream_step(&st, play_pos, 1);
            if (g_cur_bank != 0) {
                printf("    left bank %u selected after a step\n", g_cur_bank);
                ok = 0;
                break;
            }
        }
        check("streaming the whole asset yields it byte for byte", ok);
        check("the stream then reports DONE", st.state == FMT_ICM_STREAM_DONE);
    }

    /* A looping asset, placed at the very end of the card so the wrap is
     * also a jump back across three bank boundaries. */
    {
        static unsigned char ring[4096];
        fmt_icm_stream st;
        uint32_t play_pos = 0;
        const uint32_t base = CARD_BYTES - 1024;
        int ok = 1;

        fmt_icm_stream_init(&st, base, 1024, ring, sizeof ring, 1);
        while (play_pos < 4096) {
            uint32_t avail = st.fill_pos - play_pos;
            uint32_t take = avail < FMT_ICM_STEP_BYTES ? avail : FMT_ICM_STEP_BYTES;
            uint32_t j;

            for (j = 0; j < take; j++, play_pos++) {
                if (ring[play_pos & (sizeof ring - 1)] !=
                    model(base + (play_pos % 1024))) {
                    printf("    loop mismatch at %u\n", play_pos);
                    ok = 0;
                    break;
                }
            }
            if (!ok || st.state != FMT_ICM_STREAM_RUN) {
                break;
            }
            fmt_icm_stream_step(&st, play_pos, 1);
        }
        check("a looping stream repeats the asset without ending",
              ok && play_pos == 4096 && st.state == FMT_ICM_STREAM_RUN);
    }

    printf("bank register written %lu times, window left on bank %u\n",
           g_bank_writes, g_cur_bank);
    check("the window is left showing bank 0", g_cur_bank == 0);

    printf("%s\n", g_failures ? "FAILURES" : "all ok");
    return g_failures ? 1 : 0;
}
