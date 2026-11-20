/* The sampler: records into a break channel while the sequencer plays, and
 * chops what it recorded into a bank that channel plays at once.
 *
 * Source: the input (a USB interface's, through brickio: another Brick, a
 * drum machine, a record) or the output (resampling Columbia Street itself).
 * Length: 1, 2, 4 or 8 bars, starting on the next bar line while playing (the
 * recording is then exactly that long at the tempo, so it plays in time with
 * no stretching), or FREE: from the press to the next press (or stopped: on
 * the press). The input is lined up by its latency: what plays on the beat
 * at the other end, in time with this Brick's clock, lands on the beat here.
 *
 * Done, the recording is cut on a thread of its own (at its hits, on the
 * 16th grid), and swapped in under the audio lock as RECn (n the channel),
 * replacing the last one; the channel plays it from then on. It is kept in
 * rec/ (RECn_<bars>BAR.wav) and comes back with every set and start-up.
 *
 * The audio thread calls sampler_audio after the engine renders (holding the
 * audio lock); everything else is the main thread's. */
#pragma once
#include <stdint.h>

enum { SRC_IN, SRC_OUT, NSRC };
enum { REC_IDLE, REC_ARMED, REC_RECORDING, REC_CHOPPING };

void sampler_init(int rate, const char *dir); /* the recording buffer (REC_MAX_SECS), and where they are kept */
/* the input's lateness against the heard sound, in frames: the output's queue
 * and the input's (the round trip), and any trim (CS_REC_OFFSET) */
void sampler_set_latency(int frames);

/* audio thread, audio locked, after engine_render: in (stereo float, NULL or
 * silence when there is none), out (what was just made, 16-bit stereo), the
 * engine's position before the block and how far it moves a frame */
void sampler_audio(const float *in, const int16_t *out, int frames, double pos0, double steps_per_frame);

/* main thread */
int sampler_arm(int v, int bars, int src); /* 1 armed (or recording, FREE); 0 busy */
void sampler_press(void);   /* armed: called off; recording FREE: done; in time: called off */
int sampler_state(void);    /* REC_* */
int sampler_channel(void);  /* the channel being recorded into */
float sampler_progress(void); /* 0..1 of the length (FREE: of the buffer) */
float sampler_level(void);  /* the source's peak lately, 0..1 */
/* each frame: a finished recording cut and swapped in (lock / unlock around
 * the swap); 1 with msg set when one went in (or failed) */
int sampler_poll(void (*lock)(void), void (*unlock)(void), char *msg, int n);
/* cut channel v's bank again (CHOP_*), live: 1 done, 0 not a loop */
int sampler_chop(int v, int style, void (*lock)(void), void (*unlock)(void));
/* stand-in output for the host harness: no thread, the cut done at once */
void sampler_sync_mode(int on);
