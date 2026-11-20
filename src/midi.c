/* MIDI over USB (see midi.h), on brickio (lib/brickio): its reader finds and
 * reads the ports, its musical clock (mclock, wired by mclockio) follows,
 * leads and passes on the clock over MIDI and the Brick link. What is
 * Columbia Street's own is here: the notes, the CCs, the SONG page's MIDI
 * setting, and the notices. */
#define _GNU_SOURCE
#include "midi.h"
#include "engine.h"
#include "ui.h"

#include "../lib/brickio/link.h"
#include "../lib/brickio/linkpos.h"
#include "../lib/brickio/mclockio.h"
#include "../lib/brickio/midi.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ settings */

static int clock_out = 1, follow = 1, relay = 1;

void midi_set_latency(float seconds) { mclock_set_latency(seconds); }

/* the SONG page's MIDI setting over launch.sh's CS_MIDI_CLOCK / CS_MIDI_FOLLOW /
 * CS_MIDI_RELAY, into mclock (cheap: a few atomics; any thread) */
static void apply(void)
{
    int m = song.midi_mode;
    int f = follow && (m == MIDI_AUTO || m == MIDI_FOLLOW);
    int s = clock_out && (m == MIDI_AUTO || m == MIDI_SEND);
    mclock_set_mode(f && s ? MCLOCK_AUTO : f ? MCLOCK_FOLLOW : s ? MCLOCK_SEND : MCLOCK_OFF);
    mclock_set_transport(!song.midi_clock_only);
    /* AUTO, following a clock: it goes on to the other ports, from the sound as heard here */
    mclock_set_relay(relay && f && s);
}

/* ------------------------------------------------------------- notices */

static pthread_mutex_t nmx = PTHREAD_MUTEX_INITIALIZER;
static char notices[8][48];
static int n_head, n_tail;

void midi_notice(const char *fmt, const char *arg)
{
    pthread_mutex_lock(&nmx);
    if (n_head - n_tail < 8) {
        snprintf(notices[n_head % 8], sizeof notices[0], fmt, arg);
        n_head++;
    }
    pthread_mutex_unlock(&nmx);
}

static void notice_line(const char *msg) { midi_notice("%.46s", msg); }

int midi_take_notice(char *buf, int n)
{
    int got = 0;
    pthread_mutex_lock(&nmx);
    if (n_tail < n_head) {
        snprintf(buf, (size_t)n, "%s", notices[n_tail % 8]);
        n_tail++;
        got = 1;
    }
    pthread_mutex_unlock(&nmx);
    return got;
}

/* ------------------------------------------------------- the clock's host */

/* Called by mclock holding the audio lock (mclockio takes it around every
 * change), so these are plain reads and writes */
static float h_bpm(void) { return song.bpm; }
static void h_set_bpm(float b) { song.bpm = b; }
static double h_position(void) { return engine_position(); }
static void h_set_position(double s) { engine_set_position(s); }
static void h_set_rate(double r) { engine_set_rate(r); }
static int h_playing(void) { return live.playing; }
static void h_set_playing(int on) { engine_set_playing(on); }
static const ClockHost host = {h_bpm, h_set_bpm, h_position, h_set_position, h_set_rate, h_playing, h_set_playing};

static void lk(void) { ui_lock(); }
static void ul(void) { ui_unlock(); }

static int inited;
static void init_clock(int io)
{
    McioConfig c = {.host = &host, .lock = lk, .unlock = ul, .midi = io, .links = io, .notice = notice_line};
    mcio_init(&c);
    inited = 1;
}

int midi_following(void) { return mclock_following(); }
int midi_links(void) { return mcio_links(); }
int midi_ports(void) { return midi_count() + mcio_links(); }

/* ------------------------------------------------------------- parsing */

/* the notes and CCs; the clock, song position and the link's position are
 * mclockio's (from the reader's clock hook, or mcio_feed) */
static uint8_t status, data[2];
static int ndata, skip; /* skip: inside a SysEx, or a system common message's data */

static int data_len(uint8_t st)
{
    switch (st & 0xf0) {
    case 0xc0:
    case 0xd0: return 1;
    default: return 2;
    }
}

/* CCs, for the main thread (ui_midi_cc): one writer (the MIDI thread), one
 * reader */
#define CCQ 256
static volatile uint32_t ccq[CCQ];
static volatile unsigned ccq_w, ccq_r;

int midi_take_cc(int *ch, int *cc, int *v)
{
    if (ccq_r == ccq_w)
        return 0;
    __sync_synchronize();
    uint32_t m = ccq[ccq_r % CCQ];
    __sync_synchronize();
    ccq_r++;
    *ch = (int)(m >> 16 & 15), *cc = (int)(m >> 8 & 127), *v = (int)(m & 127);
    return 1;
}

static void message(void)
{
    /* notes: channels 1-4 the breaks' slices, 5 the bass (a note on at
     * velocity 0 is a note off) */
    if ((status & 0xf0) == 0x90 || (status & 0xf0) == 0x80)
        engine_note(status & 15, data[0], (status & 0xf0) == 0x90 ? data[1] : 0);
    if ((status & 0xf0) == 0xb0 && ccq_w - ccq_r < CCQ) {
        ccq[ccq_w % CCQ] = (uint32_t)(status & 15) << 16 | (uint32_t)(data[0] & 127) << 8 | (uint32_t)(data[1] & 127);
        __sync_synchronize();
        ccq_w++;
    }
}

static pthread_mutex_t pmx = PTHREAD_MUTEX_INITIALIZER;

static void feed_notes(const uint8_t *b, int n)
{
    pthread_mutex_lock(&pmx);
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xf8) /* real time: anywhere, even inside another message */
            continue;
        if (c >= 0xf0) { /* SysEx, system common: their data isn't a channel message's */
            status = 0;
            skip = c != 0xf7;
            continue;
        }
        if (c & 0x80) {
            skip = 0;
            status = c;
            ndata = 0;
            continue;
        }
        if (skip || !status)
            continue;
        data[ndata++] = c;
        if (ndata >= data_len(status)) {
            message();
            ndata = 0; /* running status: the same kind again */
        }
    }
    pthread_mutex_unlock(&pmx);
}

void midi_feed(const uint8_t *b, int n, uint64_t t)
{
    if (!inited)
        init_clock(0);
    apply();
    mcio_feed(b, n, t);
    feed_notes(b, n);
}

/* a port's bytes, on brickio's reader thread (its clock hook has had the clock already) */
static void on_raw(int dev, const uint8_t *b, int n, uint64_t t_ns, void *ud)
{
    (void)dev, (void)t_ns, (void)ud;
    feed_notes(b, n);
}

void midi_reset(void)
{
    if (!inited)
        init_clock(0);
    else
        mclock_reset();
    pthread_mutex_lock(&pmx);
    status = 0, ndata = 0, skip = 0;
    pthread_mutex_unlock(&pmx);
    ccq_r = ccq_w;
}

/* ------------------------------------------------------- clock out, link */

int midi_link_pos(uint8_t *out, int max, double heard, double bpm, int playing)
{
    return linkpos_encode(out, max, heard, bpm, playing);
}

int midi_clock_step(uint64_t t, uint8_t *out, int max)
{
    if (!inited)
        init_clock(0);
    apply();
    return mclock_step(t, out, max);
}

int midi_link_step(uint64_t t, uint8_t *out, int max) { return mclock_link_step(t, out, max); }

/* ------------------------------------------------------------- running */

static int running, link_on;

static int env_on(const char *name)
{
    const char *v = getenv(name);
    return !(v && *v == '0');
}

void midi_io_start(void)
{
    clock_out = env_on("CS_MIDI_CLOCK");
    follow = env_on("CS_MIDI_FOLLOW");
    relay = env_on("CS_MIDI_RELAY");
    init_clock(1); /* the reader's clock and SysEx hooks, the links */
    midi_reset();
    apply();
    /* the readers at real-time priority (below the sound, above the screen): each byte's time is taken as it arrives */
    midi_set_priority(40);
    midi_set_raw(on_raw, NULL);
    midi_start(NULL, NULL);
    /* the bottom port as a clock link: a Brick with a hub can lead this one (CS_LINK=0: left alone) */
    link_follow_priority(40);
    link_on = link_follow_start("CS", mcio_link_bytes, NULL, notice_line);
    mcio_start();
    running = 1;
}

void midi_io_stop(void)
{
    if (!running)
        return;
    running = 0;
    mcio_stop(); /* STOP out if the clock was going */
    if (link_on)
        link_follow_stop(); /* the bottom port given back */
    midi_set_raw(NULL, NULL);
    midi_stop();
}

/* main thread, each frame: the settings into the clock, and MIDI ports coming and going */
void midi_tick(void)
{
    static int ports_was;
    apply();
    if (!running)
        return;
    int n = midi_count();
    if (n > ports_was)
        midi_notice("MIDI: %.40s", n == 1 ? midi_name() : "ANOTHER PORT");
    else if (n < ports_was)
        midi_notice("MIDI: %.40s", "A PORT WENT");
    ports_was = n;
}
