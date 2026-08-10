/* Exercises the playback structure the MP2 stream depends on: that the
 * decoder services the DAC as it goes rather than stalling it, and that the
 * double-buffer handover around it is correct.
 *
 * The decoder is built for the host with FMT_MP2_DAC_TICK on and the hostio
 * shim underneath, so the real MP2_TICK sites, the real fmt_dac_tick() and the
 * real handover all run.  A virtual microsecond clock stands in for the TOWNS
 * free-running timer and is advanced by a flat modelled cost per tick.
 *
 * Two things this deliberately does not claim.  It is not a measurement of
 * target speed - the host's instruction mix is nothing like a 386SX's, and
 * whether a particular machine keeps up is answered by
 * fmt_mp2_stream_underruns() on the target, not here.  And the per-tick cost
 * is an assumption rather than an observation: timing the host between ticks
 * was tried first and failed, because the decoder does roughly 8ns of host
 * work between two ticks, far below the cost of the clock_gettime needed to
 * see it.
 *
 * What it does test is structural, and that is where the previous version of
 * this player went wrong: tick density (how much decode happens between two
 * opportunities to emit a sample), and whether the handover keeps the output
 * fed.  Removing tick sites, or reintroducing a decode that runs to
 * completion before the DAC is looked at again, fails it.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "kjmp2_fast.h"
#include "dacout.h"

/* Fraction of a frame's own 72ms play time that decoding it is modelled to
 * cost.  0.60 is the estimate for a 16 MHz 386SX with the current kernel. */
#ifndef MP2_DECODE_LOAD
#define MP2_DECODE_LOAD 0.60
#endif

#define FRAME_PERIOD_US 72000.0

unsigned long host_now_us;
unsigned long host_dac_writes;
unsigned char host_dac_last;
unsigned long host_dac_times[200000];
unsigned long host_dac_count;

static double vclock_us;
static double tick_cost_us;
static unsigned long ticks_total;
static unsigned long ticks_in_decode;
static unsigned long ticks_this_frame;
static unsigned long min_ticks_per_frame = ~0UL;

void host_clock_advance(void)
{
    ticks_total++;
    ticks_this_frame++;
    vclock_us += tick_cost_us;
    host_now_us = (unsigned long)vclock_us;
}

static int failures;

static void check(const char *what, int ok, const char *detail)
{
    printf("%-48s %s%s%s\n", what, ok ? "ok" : "FAIL",
           detail && *detail ? "  " : "", detail ? detail : "");
    if (!ok) failures++;
}

int main(int argc, char **argv)
{
    static uint8_t in[8u << 20];
    static uint8_t pcm[2][KJMP2_SAMPLES_PER_FRAME];
    static kjmp2v_context_t dec;
    FILE *f;
    size_t n, pos = 0;
    unsigned frames = 0, which = 0;
    char buf[160];

    if (argc != 2) { fprintf(stderr, "usage: %s in.mp2\n", argv[0]); return 2; }
    if (!(f = fopen(argv[1], "rb"))) return 2;
    n = fread(in, 1, sizeof in, f);
    fclose(f);

    /* Pass 1: count the tick sites a frame decode actually reaches, with the
     * clock frozen so nothing is emitted. */
    kjmp2v_init(&dec);
    tick_cost_us = 0.0;
    fmt_dac_start();
    fmt_dac_submit(pcm[0], KJMP2_SAMPLES_PER_FRAME);
    while (pos + 320 <= n && frames < 64) {
        unsigned long size;
        ticks_this_frame = 0;
        size = kjmp2v_decode_frame_pcm8(&dec, in + pos, pcm[1]);
        if (!size) break;
        if (ticks_this_frame < min_ticks_per_frame) min_ticks_per_frame = ticks_this_frame;
        pos += size;
        frames++;
    }
    snprintf(buf, sizeof buf, "%lu ticks per frame, %u samples per frame",
             min_ticks_per_frame, KJMP2_SAMPLES_PER_FRAME);
    check("decoder offers >=1 tick per output sample",
          min_ticks_per_frame >= KJMP2_SAMPLES_PER_FRAME, buf);

    /* Modelled cost per tick, spreading a frame's decode over its ticks. */
    tick_cost_us = (MP2_DECODE_LOAD * FRAME_PERIOD_US) / (double)min_ticks_per_frame;
    snprintf(buf, sizeof buf, "%.1fus per tick, sample period is 62.5us", tick_cost_us);
    check("decode between two ticks fits in a sample period",
          tick_cost_us < 62.5, buf);

    /* Pass 2: play the file through the real handover with the clock running. */
    kjmp2v_init(&dec);
    vclock_us = 0.0;
    host_now_us = 0;
    ticks_total = 0;
    fmt_dac_start();
    fmt_dac_submit(pcm[0], KJMP2_SAMPLES_PER_FRAME);
    host_dac_count = 0;
    host_dac_writes = 0;
    pos = 0;
    frames = 0;
    which = 0;
    while (pos + 320 <= n && frames < 500) {
        unsigned long size = kjmp2v_decode_frame_pcm8(&dec, in + pos, pcm[which ^ 1u]);
        if (!size) break;
        ticks_in_decode = ticks_total;
        /* Idle until the playing buffer is done, exactly as mp2stream.c does. */
        while (!fmt_dac_drained()) fmt_dac_tick();
        which ^= 1u;
        fmt_dac_submit(pcm[which], KJMP2_SAMPLES_PER_FRAME);
        pos += size;
        frames++;
    }

    snprintf(buf, sizeof buf, "%lu writes for %u frames (expected %u)",
             host_dac_writes, frames, frames * KJMP2_SAMPLES_PER_FRAME);
    check("every decoded sample reached the DAC exactly once",
          host_dac_writes == (unsigned long)frames * KJMP2_SAMPLES_PER_FRAME, buf);

    snprintf(buf, sizeof buf, "%lu underruns over %u frames",
             (unsigned long)fmt_dac.underruns, frames);
    check("no sample deadline missed during decode",
          fmt_dac.underruns == 0, buf);

    {
        double played_us = (double)host_dac_writes * 62.5;
        double ratio = vclock_us / played_us;
        snprintf(buf, sizeof buf, "%.1fms modelled for %.1fms of audio (%.2fx)",
                 vclock_us / 1000.0, played_us / 1000.0, ratio);
        check("playback keeps up with real time", ratio < 1.02, buf);
    }

    printf("\n%s\n", failures ? "FAILURES" : "all interleave checks passed");
    return failures != 0;
}
