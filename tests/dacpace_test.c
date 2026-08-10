/* Verifies the timer-paced DAC output in dacout.[ch]:
 *   - the average rate is exactly FMT_DAC_RATE despite the period being a
 *     non-integer number of 1us timer counts,
 *   - the 16-bit timer wrapping every 65.536ms does not disturb it,
 *   - a long stall resynchronises instead of trying to catch up at a rate
 *     the chip could not accept,
 *   - underruns are counted rather than silently dropped.
 */
#include <stdio.h>
#include <stdlib.h>
#include "dacout.h"

unsigned long host_now_us;
unsigned long host_dac_writes;
unsigned char host_dac_last;
unsigned long host_dac_times[200000];
unsigned long host_dac_count;

static unsigned char pcm[1152];
static int failures;

static void check(const char *what, int ok, const char *detail)
{
    printf("%-46s %s%s%s\n", what, ok ? "ok" : "FAIL",
           detail && *detail ? "  " : "", detail ? detail : "");
    if (!ok) failures++;
}

int main(void)
{
    unsigned long i, gaps62 = 0, gaps63 = 0, other = 0, span;
    double rate;
    char buf[160];

    for (i = 0; i < 1152; i++) pcm[i] = (unsigned char)i;

    /* ---- 1. rate accuracy over ~10 seconds of playback ---- */
    host_now_us = 12345;              /* arbitrary phase */
    fmt_dac_start();
    fmt_dac_submit(pcm, 1152);
    host_dac_count = 0; host_dac_writes = 0;

    /* Tick far more often than the sample rate, as the decoder does, and
     * keep the buffer topped up so nothing underruns. */
    for (i = 0; i < 10UL * 1000000UL; i++) {
        host_now_us = 12345 + i;      /* 1us per step */
        if (fmt_dac_drained()) fmt_dac_submit(pcm, 1152);
        fmt_dac_tick();
    }
    span = host_dac_times[host_dac_count - 1] - host_dac_times[0];
    rate = (double)(host_dac_count - 1) * 1000000.0 / (double)span;
    snprintf(buf, sizeof buf, "%.4f Hz over %lu writes", rate, host_dac_count);
    check("output rate is exactly 16 kHz", rate > 15999.9 && rate < 16000.1, buf);

    for (i = 1; i < host_dac_count; i++) {
        unsigned long d = host_dac_times[i] - host_dac_times[i - 1];
        if (d == 62) gaps62++; else if (d == 63) gaps63++; else other++;
    }
    snprintf(buf, sizeof buf, "62us x%lu, 63us x%lu, other x%lu", gaps62, gaps63, other);
    check("period alternates 62/63us, never anything else",
          other == 0 && gaps62 > 0 && gaps63 > 0 &&
          (gaps62 > gaps63 ? gaps62 - gaps63 : gaps63 - gaps62) <= 1, buf);

    snprintf(buf, sizeof buf, "%lu wraps crossed", span / 65536UL);
    check("16-bit timer wrap does not disturb pacing", span / 65536UL > 100, buf);

    /* ---- 2. underrun is counted and holds the last value ---- */
    fmt_dac_start();
    fmt_dac_submit(pcm, 4);
    for (i = 0; i < 1000; i++) { host_now_us += 1; fmt_dac_tick(); }
    snprintf(buf, sizeof buf, "%lu underruns after 4-sample buffer",
             (unsigned long)fmt_dac.underruns);
    check("starved ticks counted, last sample held",
          fmt_dac.underruns > 0 && host_dac_last == pcm[3], buf);

    /* ---- 3. a long stall resynchronises rather than machine-gunning ---- */
    fmt_dac_start();
    fmt_dac_submit(pcm, 1152);
    host_dac_count = 0;
    for (i = 0; i < 200; i++) { host_now_us += 1; fmt_dac_tick(); }
    host_now_us += 50000;             /* 50ms stall: 800 samples' worth */
    for (i = 0; i < 5000; i++) { host_now_us += 1; fmt_dac_tick(); }
    {
        unsigned long backlog = 0;
        for (i = 1; i < host_dac_count; i++)
            if (host_dac_times[i] == host_dac_times[i - 1]) backlog++;
        snprintf(buf, sizeof buf, "%lu back-to-back writes after 50ms stall", backlog);
        check("stall resynchronises, no write burst", backlog <= 1, buf);
    }

    printf("\n%s\n", failures ? "FAILURES" : "all pacing checks passed");
    return failures != 0;
}
