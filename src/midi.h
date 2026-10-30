/* MIDI over USB, from Jaffedrone: class-compliant drum machines, sequencers
 * and interfaces on the TrimUI's USB host port (the Brick's top USB-C), read
 * and written through ALSA's raw MIDI ports (/dev/snd/midiC*D*), so nothing
 * more is needed on the device. Ports are found as they are plugged in.
 *
 * In (any channel):
 *   clock          the tempo follows it, and its phase is held by trimming
 *                  the playback speed, as Wi-Fi sync does
 *   START          from the top;  CONTINUE  from where it stopped;  STOP
 *   song position  places the sequencer (in 16ths)
 *   CC             the important settings (ui_midi_cc; see docs/MIDI.md)
 * Out, to every port: clock (24 a quarter note), START and STOP, while the
 * sequencer plays and no clock is coming in. The clock goes out as the sound
 * is heard, the audio buffer's time behind the engine.
 *
 * Settings, in launch.sh: CS_MIDI=0 turns it off, CS_MIDI_CLOCK=0 sends no
 * clock, CS_MIDI_FOLLOW=0 ignores the clock coming in. */
#pragma once
#include <stdint.h>

void midi_start(int core);   /* the thread (pinned to core, -1: anywhere) */
void midi_stop(void);
void midi_set_latency(float seconds); /* from the engine's position to the sound heard */
/* messages for the screen ("MIDI: <device>"), main thread */
int midi_take_notice(char *buf, int n);
int midi_following(void);    /* an outside clock leads */
/* the next CC (for ui_midi_cc), main thread; 0 when there's none */
int midi_take_cc(int *ch, int *cc, int *v);

/* the parser and its actions without the thread or devices, for the test
 * harness: bytes in, as if from a port, at a time in microseconds */
void midi_feed(const uint8_t *b, int n, uint64_t now_us);
void midi_reset(void);
/* the clock out, stepped by hand: returns the bytes it would send now */
int midi_clock_step(uint64_t now_us, uint8_t *out, int max);
