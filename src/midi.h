/* MIDI over USB on brickio (lib/brickio): class-compliant drum machines,
 * sequencers and interfaces on the TrimUI's USB host port (the Brick's top
 * USB-C), read and written through ALSA's raw MIDI ports (/dev/snd/midiC*D*)
 * by brickio's reader, so nothing more is needed on the device. Ports are
 * found as they are plugged in. The clock, in and out and over the Brick link,
 * is brickio's musical clock (mclock, wired by mclockio: song position too);
 * the notes and CCs are parsed here.
 *
 * In (any channel):
 *   clock          the tempo follows it, and its phase is held by trimming
 *                  the playback speed, as Wi-Fi sync does
 *   START          from the top;  CONTINUE  from where it stopped;  STOP
 *   song position  places the sequencer (in 16ths)
 *   notes          channels 1-4 play breaks 1-4's slices (note 36, C1, the
 *                  first, each note up the next), channel 5 the bass at the
 *                  key's pitch, held while the key is
 *   CC             the important settings (ui_midi_cc; see docs/MIDI.md)
 * Out, to every port: clock (24 a quarter note), START and STOP, while the
 * sequencer plays: leading, or following on AUTO (passed on, to every port but
 * the one it comes in on). The clock goes out as the sound is heard: the audio
 * clock (aclock), which knows the output's real latency.
 *
 * Which way the clock goes is the SONG page's MIDI setting (song.midi_mode:
 * AUTO as above, SEND always, FOLLOW only, OFF neither). In launch.sh:
 * CS_MIDI=0 turns MIDI off, CS_MIDI_CLOCK=0 never sends clock, CS_MIDI_FOLLOW=0
 * never follows it, whatever the setting. */
#pragma once
#include <stdint.h>

#include "sync.h"
/* the engine, for brickio's Wi-Fi sync (sync_init, sync_core_init) */
const SyncClock *wifi_clock(void);

/* brickio's reader (real time), the Brick link's follower on the bottom port
 * (CS_LINK=0 leaves it alone), and the clock's own thread (every millisecond,
 * real time, mclockio). Following a clock on AUTO, the clock goes on to the
 * other ports from this Brick's heard sound (CS_MIDI_RELAY=0: not) */
void midi_io_start(void);
void midi_io_stop(void);
void midi_tick(void); /* main thread, each frame: the SONG page's setting, ports coming and going */
/* the leading Brick's sound over the link's data channel (brickio's blinkauto; Jaffedrone sends it):
 * 1 while one sends, with its name; the audio thread takes a block of it (silence and pos -1 when none),
 * with the music's position of its first frame in the sender's 16ths */
typedef struct { char s[25]; } BlinkName;
int midi_link_audio(BlinkName *name);
void midi_link_pull(float *dst, int frames, double *pos);
void midi_set_latency(float seconds); /* from the engine's position to the sound heard */
/* messages for the screen ("MIDI: <device>"), main thread */
int midi_take_notice(char *buf, int n);
int midi_following(void);    /* an outside clock leads */
int midi_ports(void);        /* the MIDI ports open, links included (for the screen) */
int midi_links(void);        /* Bricks following this one over the USB clock link (link.h) */
/* a message for the screen, any thread (fmt with one %s) */
void midi_notice(const char *fmt, const char *arg);
/* the next CC (for ui_midi_cc), main thread; 0 when there's none */
int midi_take_cc(int *ch, int *cc, int *v);

/* the parser and its actions without the thread or devices, for the test
 * harness: bytes in, as if from a port, at a time in microseconds */
void midi_feed(const uint8_t *b, int n, uint64_t now_us);
void midi_reset(void);
/* the leading Brick's position for the ones following over the link: its
 * bytes (F0 7D 'T' 'L' ... F7, see midi.c), and when they are due (every 20 ms
 * while the clock goes out; 0 when not) */
int midi_link_pos(uint8_t *out, int max, double heard, double bpm, int playing);
int midi_link_step(uint64_t now_us, uint8_t *out, int max);
/* the clock out, stepped by hand: returns the bytes it would send now */
int midi_clock_step(uint64_t now_us, uint8_t *out, int max);
