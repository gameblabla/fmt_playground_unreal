#ifndef FMT_PCMSTREAM_H
#define FMT_PCMSTREAM_H

#include <stdint.h>

/*
 * CD PCM playback via the YM2612 channel-6 DAC - separate from
 * fmt_sound_play() (sound.[ch], RF5C68 wave-table playback) and from
 * CD-DA (audio-track playback handled entirely by the CD-ROM drive's
 * own DAC, never touching the CPU). This is a raw unsigned 8-bit mono
 * PCM *file* on the data track, decoded/output entirely in software by
 * bit-banging the YM2612's channel-6 DAC one sample at a time - the
 * same mechanism FMTOWNS_32KHZPCM_WITHDAC/src/dac_pcm.c uses, ported
 * onto this project's outb()/inb().
 *
 * fmt_pcm_stream_play() streams the file straight off the disc: it
 * primes a ring buffer, then plays out of it while advancing a
 * non-blocking CD reader (fmt_cdrom_stream_step(), cdrom.[ch]) inside
 * the same loop, during the ~30us the YM2612 is busy after each sample.
 * Nothing is ever preloaded, so a file of any length plays - the
 * previous version read the whole thing into a 512KB RAM buffer first,
 * which capped playback at about 47 seconds and spent the whole load
 * time silent.
 *
 * The playback loop masks interrupts (cli) around its busy-wait pacing,
 * matching dac_pcm.c's own _disable() - an IRQ landing between two DAC
 * writes would otherwise steal real time the pacing loop assumes is
 * going entirely to the busy window. It deliberately does not unmask
 * them afterwards; see disable_interrupts() in pcmstream.c for why.
 */

/* Sample rate the PCM file must be authored at.
 *
 * This is not a free choice: the loop writes one byte per DAC write and
 * each write is paced by the YM2612's busy flag, which holds for a
 * fixed ~30us on real hardware (and in TOWNSEMU, see dacBusyUntil), so
 * the output rate is whatever that pacing gives - about 32kHz, measured
 * at 32.3kHz under TOWNSEMU. FMTOWNS_32KHZPCM_WITHDAC authors its clip
 * at 32000Hz for exactly this reason and this project follows it: one
 * file sample == one hardware DAC write, no resampling on the Towns
 * side at all. Anything else has to be stretched by the playback loop,
 * and a zero-order hold is the only stretch cheap enough to do between
 * DAC writes - which sounds like what it is.
 *
 * Author with: sox <input> -r 32000 -c 1 -b 8 -e unsigned-integer \
 *                  -t raw CD/MUSIC.PCM gain -n -1 */
#define FMT_PCM_STREAM_RATE     32000u /* unsigned 8-bit mono samples/sec */

/* Looks up `name` on the CD (via iso9660.c) and streams it out through
 * the YM2612 DAC, looping at the end of the file. Blocking, and does
 * not return on its own - there's no interrupt-driven playback in this
 * environment, so nothing else runs while this is going (same as
 * dac_pcm.c's own design), and the only way out is the drive reporting
 * a failure. Returns 0 if playback ended that way, -1 if the file
 * wasn't found or nothing could be read from it. */
int fmt_pcm_stream_play(const char *name);

#endif
