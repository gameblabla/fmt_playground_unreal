#ifndef FMT_DACOUT_H
#define FMT_DACOUT_H

#include <stdint.h>
#include "io.h"

/*
 * Timer-paced output to the YM2612 channel-6 DAC.
 *
 * Why this exists, rather than the busy-flag pacing pcmstream.c uses:
 * that loop writes a sample, then spins until the YM2612 clears its busy
 * flag, and takes whatever rate that produces as the sample rate.  The busy
 * flag is not a sample clock.  Tsugaru models it as a flat 30us
 * (YM2612_DATA_WRITE_BUSY_NS in towns/sound/sound.cpp), which is where the
 * "about 32kHz" figure in pcmstream.h comes from - but on a real YM2612 the
 * flag is a register-write interlock lasting a few microseconds, so the same
 * loop runs far faster on real hardware than in the emulator.  A stream
 * authored at one rate and played back at "whatever the busy flag gives on
 * this machine" is off by different amounts on every target.
 *
 * The FM TOWNS has an actual clock for this: a free-running 16-bit counter at
 * I/O 0x26 that increments once per microsecond and wraps every 65.536ms.
 * Pacing against it makes the output rate exact and identical on hardware and
 * emulator, and it costs one word read per tick instead of an open-ended spin.
 *
 * The other half of the design is that playback is driven by polling, from
 * fmt_dac_tick(), which is called both from the idle loop and from inside the
 * MP2 decoder's inner loops (see MP2_TICK in kjmp2_fast.c).  Decoding one
 * frame takes tens of milliseconds - far longer than a sample period - so a
 * decoder that runs to completion between two DAC writes stalls the output for
 * as long as it runs.  Scattering ticks through the decode instead means the
 * decode happens in the gaps between samples rather than in place of them.
 * Every tick site is chosen to be less than one sample period of work apart.
 */

#define FMT_DAC_FREERUN_TIMER   0x26  /* 16-bit, 1us/count, free-running */

#define FMT_DAC_YM_ADDR0        0x4D8
#define FMT_DAC_YM_DATA0        0x4DA
#define FMT_DAC_YM_REG_DAC_ENABLE 0x2B
#define FMT_DAC_YM_REG_DAC_DATA   0x2A
#define FMT_DAC_YM_BUSY         0x80
#define FMT_DAC_SOUND_MUTE      0x4D5
#define FMT_DAC_SOUND_AUDIO     0x4EC

/* Source rate of the decoded stream.  The DAC is written once per source
 * sample: the asset is authored at this rate, so there is no resampling and
 * no zero-order-hold stretch in the output path at all.  62.5us per sample
 * is not an integer number of timer counts, so the half-microsecond is
 * carried in `frac` and the period alternates 62, 63, 62, 63 - exact on
 * average, with one microsecond of jitter on a 62.5us period. */
#define FMT_DAC_RATE            16000u
#define FMT_DAC_PERIOD_US       62u
#define FMT_DAC_PERIOD_FRAC     1u    /* plus 1/2 us */

/* If the tick is starved for longer than this, the backlog is abandoned and
 * the deadline resynchronised to now.  Without it, a long stall would leave
 * the tick trying to catch up by writing back-to-back faster than the chip
 * accepts.  Four periods of slip is inaudible; compounding it is not. */
#define FMT_DAC_RESYNC_US       250

typedef struct {
    const uint8_t *buf;      /* decoded samples currently being played out */
    uint16_t       pos;      /* next sample index within buf */
    uint16_t       len;      /* samples in buf */
    uint16_t       deadline; /* free-run timer value the next write is due at */
    uint8_t        frac;     /* carried half-microseconds, 0 or 1 */
    uint8_t        active;
    uint8_t        last;     /* last byte written, held across an underrun */
    uint32_t       underruns;/* samples the decoder failed to supply in time */
} fmt_dac_state;

extern fmt_dac_state fmt_dac;

/* Emit one sample if its deadline has passed.  Cheap and safe to call far
 * more often than the sample rate - the common case is one word read from
 * the timer and a not-taken branch. */
static inline void fmt_dac_tick(void)
{
    uint16_t now;
    unsigned step;

    if (!fmt_dac.active) {
        return;
    }
    now = inw(FMT_DAC_FREERUN_TIMER);
    if ((int16_t)(now - fmt_dac.deadline) < 0) {
        return;  /* not due yet */
    }

    if (fmt_dac.pos < fmt_dac.len) {
        fmt_dac.last = fmt_dac.buf[fmt_dac.pos++];
    } else {
        /* Nothing decoded yet: hold the last value rather than emit a step
         * to silence, and count it so the caller can see it happened. */
        fmt_dac.underruns++;
    }
    outb(fmt_dac.last, FMT_DAC_YM_DATA0);

    step = FMT_DAC_PERIOD_US;
    fmt_dac.frac += FMT_DAC_PERIOD_FRAC;
    if (fmt_dac.frac >= 2u) {
        fmt_dac.frac -= 2u;
        step++;
    }
    fmt_dac.deadline += (uint16_t)step;
    if ((int16_t)(now - fmt_dac.deadline) > FMT_DAC_RESYNC_US) {
        fmt_dac.deadline = now;
    }
}

/* Unmutes the FM/PCM path, switches channel 6 to DAC mode and latches the DAC
 * data register, then starts the clock.  Interrupts are left masked; see the
 * note in pcmstream.c for why this payload never unmasks them. */
void fmt_dac_start(void);

/* Hands the tick a new buffer of decoded samples.  Called between frames;
 * the caller must not touch the previous buffer afterwards, since the tick
 * may still have been reading it right up to this point. */
void fmt_dac_submit(const uint8_t *buf, uint16_t len);

/* True once the current buffer has been played out. */
static inline int fmt_dac_drained(void)
{
    return fmt_dac.pos >= fmt_dac.len;
}

/* Restores channel 6 to FM synthesis and stops the clock. */
void fmt_dac_stop(void);

#endif
