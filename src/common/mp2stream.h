#ifndef FMT_MP2STREAM_H
#define FMT_MP2STREAM_H

/* CD-streamed MPEG-2 Layer II (mono 16 kHz, 32 kbit/s) playback through
 * the YM2612 DAC.  Like pcmstream, playback is blocking/polled. */
int fmt_mp2_stream_load_file(const char *name);
int fmt_mp2_stream_play_streaming(void);
void fmt_mp2_stream_stop(void);

#endif
