/* Headless host harness: plays a scripted button sequence over the real
 * samples, writes a WAV of the audio and a screenshot of each page, then
 * checks the engine: straight break playback, bit depth, compressor,
 * pattern generation, save / load and DSP time; on the TrimUI's side, USB
 * MIDI clock in and out and the Brick Pro's sticks.
 *
 * usage: cs_test OUTDIR [APP_DIR]   (APP_DIR holds breaks/ and bass/) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aclock.h"
#include "breaks.h"
#include "engine.h"
#include "gfx.h"
#include "leds.h"
#include "midi.h"
#include "perflog.h"
#include "sync.h"
#include "ui.h"
#include "update.h"

#define RATE 48000

typedef struct {
    int ms, btn, down;
    const char *shot;
} Ev;

#define PRESS(t, b) {t, b, 1, NULL}, {t + 60, b, 0, NULL}
#define SHOT(t, name) {t, -1, 0, name}

static const Ev script[] = {
    PRESS(0, BTN_START),
    SHOT(3000, "pattern"),
    /* punch-in effects: repeat 1/16, reverse, tape stop */
    {4000, BTN_R2, 1, NULL},
    {4050, BTN_RIGHT, 1, NULL},
    SHOT(4600, "fx_repeat"),
    {5200, BTN_RIGHT, 0, NULL},
    {5300, BTN_LEFT, 1, NULL},
    {6300, BTN_LEFT, 0, NULL},
    {6400, BTN_DOWN, 1, NULL},
    {7600, BTN_DOWN, 0, NULL},
    {7650, BTN_R2, 0, NULL},
    /* L2 sweep */
    {8000, BTN_L2, 1, NULL},
    SHOT(9000, "filter"),
    {10000, BTN_L2, 0, NULL},
    /* pattern edits: BRK3 ALT cell, more hits, B+right rotates, B tap re-rolls */
    PRESS(10500, BTN_DOWN), PRESS(10600, BTN_DOWN), PRESS(10700, BTN_RIGHT), PRESS(10800, BTN_RIGHT),
    PRESS(10900, BTN_X),
    {11000, BTN_B, 1, NULL}, PRESS(11050, BTN_RIGHT), {11200, BTN_B, 0, NULL},
    SHOT(11400, "pattern_edit"),
    /* CHOP page: BRK2, bits down to 6, compressor up, filter */
    PRESS(12000, BTN_R1), PRESS(12100, BTN_RIGHT),
    PRESS(12200, BTN_UP), PRESS(12300, BTN_UP), PRESS(12400, BTN_UP), /* -> BITS */
    {12500, BTN_Y, 1, NULL}, {13600, BTN_Y, 0, NULL},
    PRESS(13700, BTN_DOWN), PRESS(13800, BTN_DOWN), /* -> COMP */
    {13900, BTN_X, 1, NULL}, {15200, BTN_X, 0, NULL},
    SHOT(16000, "chop"),
    /* BASS page: riff up */
    PRESS(16500, BTN_R1), PRESS(16600, BTN_DOWN), PRESS(16700, BTN_DOWN), PRESS(16800, BTN_DOWN),
    PRESS(16900, BTN_X),
    SHOT(18000, "bass"),
    /* MIX page: master glue up */
    PRESS(18500, BTN_R1), PRESS(18600, BTN_LEFT), PRESS(18700, BTN_DOWN),
    PRESS(18800, BTN_X), PRESS(18900, BTN_X),
    SHOT(20000, "mix"),
    /* SONG page: save to A, a new beat, save to B, queue A */
    PRESS(20500, BTN_R1), PRESS(20600, BTN_X),
    PRESS(21000, BTN_SELECT), PRESS(21100, BTN_RIGHT), PRESS(21200, BTN_X),
    PRESS(21400, BTN_LEFT), PRESS(21500, BTN_A),
    SHOT(21600, "song"),
    /* the generated beat on the pattern page */
    PRESS(23000, BTN_R1),
    PRESS(23200, BTN_SELECT),
    SHOT(25000, "pattern_generated"),
    PRESS(29900, BTN_START),
};
#define NSCRIPT ((int)(sizeof script / sizeof script[0]))
#define TOTAL_MS 30000

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void write_wav(const char *path, const int16_t *pcm, int frames)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, RATE);
    put32(f, RATE * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, frames * 4);
    fwrite(pcm, 4, frames, f);
    fclose(f);
}

static void write_ppm(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint8_t px[3] = {(uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i]};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static uint32_t fb[SCREEN_W * SCREEN_H];

/* silence every channel but v, with plain settings */
static void solo_channel(int v)
{
    for (int i = 0; i < NV; i++) {
        song.pat.mute[i] = i != v;
        song.solo[i] = 0;
        for (int r = 0; r < NROW; r++)
            song.pat.cell[i][r].on = 0;
    }
    song.send[v] = 0;
    song.pan[v] = 0;
    song.vol[v] = 100;
    song.master = 77; /* gain = 0.77^2 * 1.3 ~ 0.77 */
    song.master_drive = 0;
    song.glue = 0;
    song.verb_time = 0;
}

static double corr(const float *a, const float *b, int n)
{
    double ab = 0, aa = 1e-12, bb = 1e-12;
    for (int i = 0; i < n; i++) {
        ab += a[i] * b[i];
        aa += a[i] * a[i];
        bb += b[i] * b[i];
    }
    return ab / sqrt(aa * bb);
}

/* render n frames to a mono float buffer (left channel) */
static UpdateStatus fake_update; /* the update prompt, as if v9.9.9 were out */
static void fake_install(void)
{
    fake_update.state = UP_INSTALLING;
    fake_update.progress = 42;
}

static void render_mono(float *dst, int n)
{
    static int16_t buf[2048 * 2];
    for (int done = 0; done < n;) {
        int k = n - done < 1024 ? n - done : 1024;
        engine_render(buf, k);
        for (int i = 0; i < k; i++)
            dst[done + i] = buf[2 * i] / 32768.f;
        done += k;
    }
}

/* Wi-Fi sync over a simulated network: a leader 1.7 steps ahead whose
 * packets arrive 3 ms late at best and up to 40 ms later at random (one in
 * twenty held 100 ms, as for a power-saving unit), read only every 33 ms,
 * with the audio made in 1024-frame buffers. echo: the leader echoes our
 * packets (a current unit), or not (an older one). Returns how far the
 * follower is heard from the leader over the last 4 of 20 s, in ms. */
static unsigned sim_rand = 12345;
static double sim_jitter(void)
{
    sim_rand = sim_rand * 1103515245u + 12345u;
    double u = (sim_rand >> 8) / 16777216.0;
    return 3000.0 + (u < 0.05 ? 100000.0 : u * 40000.0);
}

typedef struct {
    uint8_t b[SYNC_PKT_LEN];
    uint64_t at;
    int live;
} SimPkt;

static void sim_sync(int echo, double *mean_ms, double *worst_ms, float *delay)
{
    const double bpm = 120.0, spu = bpm / 15e6; /* 16ths per us */
    const uint64_t off = 987654321ULL;           /* the leader's clock runs elsewhere */
    const int buf = 1024;
    song_default();
    engine_init(RATE);
    song.bpm = 97.f;
    song.sync_mode = SYNC_FOLLOW;
    sync_core_init(100);
    aclock_init(RATE, 0);
    static int16_t pcm[2 * 1024];
    SimPkt q[64];
    memset(q, 0, sizeof q);
    uint64_t heard_mine = 0, heard_mine_at = 0;
    double sum = 0, worst = 0;
    int n = 0;
    uint64_t next_cb = 0, next_poll = 0, next_lead = 7000, next_mine = 57000;
    for (uint64_t t = 0; t < 20000000ULL; t += 250) {
        double lead_pos = 1.7 + spu * (double)t, wire_pos = lead_pos + SYNC_WIRE_LEAD_US * spu;
        if (t >= next_cb) { /* the audio device asks for a buffer */
            aclock_request(t, buf);
            engine_render(pcm, buf);
            aclock_made(buf, engine_position(), engine_steps_per_frame());
            next_cb += (uint64_t)(buf * 1e6 / RATE);
        }
        if (t >= next_lead) { /* the leader sends */
            next_lead += 100000;
            for (int i = 0; i < 64; i++)
                if (!q[i].live) {
                    uint8_t *b = q[i].b;
                    uint64_t ts = t + off;
                    float fb = (float)bpm;
                    uint32_t id = 7;
                    memset(b, 0, sizeof q[i].b);
                    memcpy(b, "HI5S", 4);
                    b[4] = 1, b[5] = 1, b[6] = SYNC_LEAD;
                    memcpy(b + 8, &id, 4);
                    memcpy(b + 12, &fb, 4);
                    memcpy(b + 16, &wire_pos, 8);
                    memcpy(b + 24, &ts, 8);
                    if (echo && heard_mine_at && t - heard_mine_at < 1000000) {
                        uint32_t me = 100, hold = (uint32_t)(t - heard_mine_at);
                        b[7] = 1;
                        memcpy(b + 32, &me, 4);
                        memcpy(b + 36, &heard_mine, 8);
                        memcpy(b + 44, &hold, 4);
                    }
                    q[i].at = t + (uint64_t)sim_jitter();
                    q[i].live = 1;
                    break;
                }
        }
        if (t >= next_mine) { /* we send; the leader hears it a trip later */
            next_mine += 100000;
            uint8_t b[SYNC_PKT_LEN];
            sync_make_packet(b, t);
            memcpy(&heard_mine, b + 24, 8);
            heard_mine_at = t + (uint64_t)sim_jitter();
        }
        if (t >= next_poll) { /* the screen's frame reads what has come in */
            next_poll += 33000;
            for (int i = 0; i < 64; i++)
                if (q[i].live && q[i].at <= t) {
                    sync_handle_packet(q[i].b, SYNC_PKT_LEN, q[i].at);
                    q[i].live = 0;
                }
        }
        if (t >= 16000000ULL && t % 10000 == 0) {
            double mine;
            if (aclock_position(t, &mine)) {
                double e = fmod(lead_pos - mine, 16.0);
                if (e > 8)
                    e -= 16;
                if (e <= -8)
                    e += 16;
                e = fabs(e / spu / 1000.0);
                sum += e;
                worst = e > worst ? e : worst;
                n++;
            }
        }
    }
    *mean_ms = n ? sum / n : 1e9;
    *worst_ms = worst;
    *delay = sync_status()->delay_ms;
    engine_set_rate(1.0);
    sync_core_init(1);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--midi-chart")) { /* the CC table of docs/MIDI.md */
        song_default();
        ui_midi_chart(stdout);
        return 0;
    }
    const char *out = argc > 1 ? argv[1] : "build/test";
    const char *app = argc > 2 ? argv[2] : "dist/ColumbiaStreet";
    char path[512];

    engine_init(RATE);
    snprintf(path, sizeof path, "%s/breaks", app);
    int nb = breaks_load_dir(path);
    snprintf(path, sizeof path, "%s/bass", app);
    int ns = bass_load_dir(path);
    if (!nbanks)
        breaks_fallback();
    printf("samples: %d banks, %d bass\n", nb, ns);
    for (int i = 0; i < nbanks; i++)
        printf("bank %-6s %5.1f BPM %2d steps %2d slices (kick %d, snare %d, hat %d)\n", banks[i].name, banks[i].bpm,
               banks[i].steps, banks[i].nslices, bank_count_tag(&banks[i], TAG_KICK),
               bank_count_tag(&banks[i], TAG_SNARE), bank_count_tag(&banks[i], TAG_HAT));

    /* ---- the scripted session */
    song_default();
    ui_init();
    sync_core_init(1);
    int frames = TOTAL_MS * RATE / 1000;
    int16_t *pcm = malloc(frames * 4);
    int ev = 0, block = RATE / 100; /* 10 ms */
    double dsp_s = 0.0;
    for (int ms = 0, pos = 0; ms < TOTAL_MS; ms += 10, pos += block) {
        while (ev < NSCRIPT && script[ev].ms <= ms) {
            const Ev *e = &script[ev++];
            if (e->shot) {
                ui_draw(fb);
                snprintf(path, sizeof path, "%s/%s.ppm", out, e->shot);
                write_ppm(path, fb);
            }
            else
                ui_button(e->btn, e->down);
        }
        ui_tick(10);
        clock_t t0 = clock();
        engine_render(pcm + pos * 2, block);
        dsp_s += (double)(clock() - t0) / CLOCKS_PER_SEC;
    }
    snprintf(path, sizeof path, "%s/session.wav", out);
    write_wav(path, pcm, frames);
    int full = 0;
    double rms = 0;
    for (int i = 0; i < frames * 2; i++) {
        full += pcm[i] >= 32767 || pcm[i] <= -32768;
        rms += (double)pcm[i] * pcm[i];
    }
    rms = sqrt(rms / (frames * 2)) / 32768.0;
    printf("session: %.0f s, rms %.3f, %d full-scale samples (want 0)\n", TOTAL_MS / 1000.0, rms, full);
    printf("dsp: %.1f%% of real time on this host\n", 100.0 * dsp_s / (TOTAL_MS / 1000.0));
    free(pcm);

    /* ---- GRID with a full HIT cell at the break's own tempo plays it straight */
    int amen = -1;
    for (int i = 0; i < nbanks; i++)
        if (!strcmp(banks[i].name, "AMEN"))
            amen = i;
    int gb = amen >= 0 ? amen : 0;
    const Bank *b = &banks[gb];
    engine_set_playing(0);
    song_default();
    solo_channel(0);
    int *P = song.pat.param[0];
    P[B_BANK] = gb;
    P[B_MODE] = MODE_GRID;
    P[B_FIT] = 1;
    P[B_LEN] = 100;
    P[B_COMP] = 0;
    song.pat.cell[0][ROW_HIT] = (Cell){.on = 1, .steps = 16, .hits = 16, .rot = 0};
    song.bpm = b->bpm;
    {
        /* every slice plays once, and the first matches the loop exactly */
        float *y = malloc(b->len * sizeof(float));
        int seen[MAX_SLICES] = {0}, played = 0, last = -1;
        static int16_t buf[512];
        engine_set_playing(1);
        for (int i = 0; i < b->len; i += 256) {
            int k = b->len - i < 256 ? b->len - i : 256;
            engine_render(buf, k);
            for (int j = 0; j < k; j++)
                y[i + j] = buf[2 * j] / 32768.f;
            if (live.slice[0] != last) {
                last = live.slice[0];
                played += !seen[last];
                seen[last] = 1;
            }
        }
        engine_set_playing(0);
        double c = corr(y + 64, b->data + 64, b->start[1] - 128);
        /* slices sharing a step play as one */
        int want = 0;
        for (int i = 0; i < b->nslices; i++)
            want += i == 0 || floorf(b->at[i] + 0.5f) != floorf(b->at[i - 1] + 0.5f);
        printf("grid straight: %s at %.0f BPM, %d of %d steps with onsets played, first slice correlation %.3f "
               "(want all and over 0.99: %d)\n",
               b->name, b->bpm, played, want, c, played == want && c > 0.99);
        free(y);
    }

    /* ---- FIT: at double the tempo the same slice plays twice as fast */
    {
        song.bpm = b->bpm * 2.f;
        P[B_MODE] = MODE_FIX;
        P[B_SLICE] = 0;
        song.pat.cell[0][ROW_HIT] = (Cell){.on = 1, .steps = 16, .hits = 1, .rot = 0};
        int n = b->start[1] - b->start[0];
        float *y = malloc(n * sizeof(float));
        engine_set_playing(1);
        render_mono(y, n);
        engine_set_playing(0);
        int last = 0;
        for (int i = 0; i < n; i++)
            if (fabsf(y[i]) > 1e-4f)
                last = i;
        printf("fit: slice of %d samples played in %d at x2 tempo (want about half: %d)\n", n, last,
               last > n * 0.4 && last < n * 0.6);
        free(y);
    }

    /* ---- bit depth: a 4-bit slice has at most 16 levels, 1 bit three */
    song.bpm = 170.f;
    for (int bits = 1; bits <= 24; bits += bits < 4 ? 3 : bits < 16 ? 4 : 8) {
        P[B_BITS] = bits;
        P[B_SLICE] = 0;
        engine_set_playing(0);
        float *y = malloc(RATE / 4 * sizeof(float));
        engine_audition(0);
        render_mono(y, RATE / 4);
        int levels = 0;
        float seen[4096];
        for (int i = 0; i < RATE / 4 && levels < 4096; i++) {
            int dup = 0;
            for (int k = 0; k < levels && !dup; k++)
                dup = seen[k] == y[i];
            if (!dup)
                seen[levels++] = y[i];
        }
        printf("bits %2d: %d output levels\n", bits, levels);
        free(y);
    }
    P[B_BITS] = 24;

    /* ---- compressor: COMP 100 pulls the peak down and reports reduction */
    {
        float peak[2], gr = 0.f;
        for (int k = 0; k < 2; k++) {
            P[B_COMP] = k ? 100 : 0;
            float *y = malloc(RATE / 2 * sizeof(float));
            engine_audition(0);
            for (int i = 0; i < RATE / 2; i += 1024) {
                render_mono(y + i, RATE / 2 - i < 1024 ? RATE / 2 - i : 1024);
                if (k && live.gr[0] > gr)
                    gr = live.gr[0];
            }
            float rmsv = 0.f;
            for (int i = 0; i < RATE / 2; i++)
                rmsv += y[i] * y[i];
            peak[k] = sqrtf(rmsv / (RATE / 2));
            free(y);
        }
        printf("comp: rms %.3f -> %.3f with COMP 100, up to %.1f dB gain reduction (want over 6: %d)\n", peak[0],
               peak[1], gr, gr > 6.f);
        P[B_COMP] = 0;
    }

    /* ---- generator: in range, and different seeds differ */
    {
        Pattern a, c;
        int same = 0, bad = 0;
        for (unsigned s = 1; s < 200; s++) {
            pattern_generate(&a, s);
            pattern_generate(&c, s + 1000);
            same += !memcmp(&a, &c, sizeof a);
            song.pat = a;
            Pattern before = song.pat;
            song_clamp();
            bad += memcmp(&before, &song.pat, sizeof before) != 0;
        }
        printf("generator: 199 seeds, %d identical pairs, %d out of range (want 0 0)\n", same, bad);
    }

    /* ---- save and load keep the pattern, banks by name */
    {
        song_default();
        song.pat.param[2][B_BITS] = 7;
        song.bpm = 163.5f;
        slot_save(3);
        snprintf(path, sizeof path, "%s/test.sav", out);
        int ok = song_save(path);
        Song before = song;
        song_default();
        ok &= song_load(path);
        printf("save/load: %d, same %d, bpm %.1f, bits %d, slot D used %d\n", ok,
               !memcmp(&before.pat, &song.pat, sizeof song.pat), song.bpm, song.pat.param[2][B_BITS],
               song.slot_used[3]);
    }

    /* ---- a v0.1 save (without the output settings) still loads, in stereo */
    {
        song_default();
        song.bpm = 151.f;
        song.out_mode = OUT_MONO_CLOCK;
        song_save(path);
        FILE *f = fopen(path, "rb");
        static unsigned char buf[sizeof(Song) + 4096];
        size_t got = fread(buf, 1, sizeof buf, f);
        fclose(f);
        uint32_t v1 = (uint32_t)offsetof(Song, out_mode);
        memcpy(buf + 4, &v1, 4);
        memmove(buf + v1, buf + sizeof(Song), got - sizeof(Song)); /* drop the new fields */
        char p1[600];
        snprintf(p1, sizeof p1, "%s/v01.sav", out);
        f = fopen(p1, "wb");
        fwrite(buf, 1, got - (sizeof(Song) - v1), f);
        fclose(f);
        song_default();
        int ok = song_load(p1);
        printf("save compat v0.1: %d, bpm %.0f, output %d, ppqn %d, pulse %d\n", ok, song.bpm, song.out_mode,
               clk_ppqn_value(song.clk_ppqn), song.clk_width);
    }

    /* ---- mono + clock: the pulses' count, spacing and width on the clock
     * side, nothing but pulses there, and the sound on the other side */
    {
        int16_t *pc = malloc(sizeof(int16_t) * 2 * 48000 * 4);
        static const int idx[4] = {1, 5, 6, 1}; /* 2, 24, 32 PPQN; the last on the left */
        for (int k = 0; k < 4; k++) {
            song_default();
            song.bpm = 120.f;
            song.pan[0] = -100, song.pan[1] = 100;
            song.out_mode = OUT_MONO_CLOCK;
            song.clk_ppqn = idx[k];
            song.clk_side = k == 3;
            engine_set_playing(0);
            engine_render(pc, 4800);
            engine_set_playing(1);
            engine_render(pc, 48000 * 4);
            int cs = k == 3 ? 0 : 1, edges = 0, prev = 0, width = 0, last = -1, other = 0;
            double gaps = 0;
            float apk = 0.f;
            for (int i = 0; i < 48000 * 4; i++) {
                int c = pc[2 * i + cs], hi = c > 16000;
                other += c != 0 && c != 31128;
                if (hi && !prev) {
                    if (last >= 0)
                        gaps += i - last;
                    last = i, edges++;
                }
                if (hi && edges == 1)
                    width++;
                prev = hi;
                if (abs(pc[2 * i + 1 - cs]) > apk)
                    apk = (float)abs(pc[2 * i + 1 - cs]);
            }
            printf("clock out: %d PPQN at 120 BPM on the %s -> %d pulses in 4 s, mean gap %.2f ms, width %.2f ms, "
                   "other values %d, sound peak %.2f\n",
                   clk_ppqn_value(idx[k]), cs ? "right" : "left", edges, gaps / (edges - 1) * 1000.0 / 48000,
                   width * 1000.0 / 48000, other, apk / 32768.f);
        }
        engine_set_playing(0);
        engine_render(pc, 24000);
        int any = 0;
        for (int i = 0; i < 24000; i++)
            any += pc[2 * i] > 16000; /* the last pass had the clock on the left */
        printf("clock out stopped: pulses %d (want 0)\n", any);
        free(pc);
        song_default();
    }

    /* ---- every bass voice sounds */
    {
        song_default();
        solo_channel(V_BASS);
        int quiet = 0;
        for (int w = 0; w < NWAVE_SYNTH + nbass; w++) {
            song.pat.param[V_BASS][S_WAVE] = w;
            song.pat.param[V_BASS][S_LEN] = 8;
            float *y = malloc(RATE / 4 * sizeof(float));
            engine_audition(V_BASS);
            render_mono(y, RATE / 4);
            float pk = 0.f;
            for (int i = 0; i < RATE / 4; i++)
                pk = fmaxf(pk, fabsf(y[i]));
            quiet += pk < 0.05f;
            free(y);
            for (int k = 0; k < 4; k++) { /* let it ring out */
                static int16_t tmp[4096];
                engine_render(tmp, 2048);
            }
        }
        printf("bass voices: %d, %d silent (want 0)\n", NWAVE_SYNTH + nbass, quiet);
    }

    /* ---- SELECT keeps the mutes and the mixer; so does its undo */
    {
        song_default();
        song.pat.mute[1] = song.pat.mute[V_BASS] = 1;
        song.solo[2] = 1;
        song.vol[0] = 33;
        song.pan[3] = -45;
        song.send[1] = 61;
        song.master = 71;
        song.glue = 12;
        song.master_drive = 27;
        song.verb_time = 64;
        song.verb_tone = 22;
        song.sweep_rate = 1;
        song.out_mode = 1;
        song.clk_ppqn = 5;
        Song mix = song;
        Pattern before = song.pat;
        ui_button(BTN_SELECT, 1);
        ui_button(BTN_SELECT, 0);
        int new_beat = memcmp(song.pat.cell, before.cell, sizeof before.cell) != 0;
        int mutes = !memcmp(song.pat.mute, mix.pat.mute, sizeof mix.pat.mute);
        int mixer = !memcmp(song.vol, mix.vol, sizeof mix.vol) && !memcmp(song.pan, mix.pan, sizeof mix.pan) &&
                    !memcmp(song.send, mix.send, sizeof mix.send) && !memcmp(song.solo, mix.solo, sizeof mix.solo) &&
                    song.master == mix.master && song.glue == mix.glue && song.master_drive == mix.master_drive &&
                    song.verb_time == mix.verb_time && song.verb_tone == mix.verb_tone &&
                    song.sweep_rate == mix.sweep_rate && song.bpm == mix.bpm && song.out_mode == mix.out_mode &&
                    song.clk_side == mix.clk_side && song.clk_ppqn == mix.clk_ppqn && song.clk_width == mix.clk_width;
        /* unmute one on the new beat, then undo: the undo keeps that too */
        song.pat.mute[1] = 0;
        ui_button(BTN_R2, 1);
        ui_button(BTN_SELECT, 1);
        ui_button(BTN_SELECT, 0);
        ui_button(BTN_R2, 0);
        int undone = !memcmp(song.pat.cell, before.cell, sizeof before.cell);
        int undo_mutes = song.pat.mute[1] == 0 && song.pat.mute[V_BASS] == 1;
        printf("select: new beat %d, mutes kept %d, mixer kept %d; undo back %d, mutes kept %d\n", new_beat, mutes,
               mixer, undone, undo_mutes);
    }

    /* ---- mute scenes: save the mutes and solos on MIX (R2 + L1), change
     * them, F1 switches the scene on and back off; R2 + F2 saves M2 anywhere;
     * an empty scene changes nothing; the scenes survive a save and load */
    {
        song_default();
        /* the defaults: M1 the bass alone, M2 the breaks alone */
        int def1 = song.scene_used[0], def2 = song.scene_used[1];
        for (int v = 0; v < NV; v++) {
            def1 &= song.scene_mute[0][v] == (v != V_BASS) && !song.scene_solo[0][v];
            def2 &= song.scene_mute[1][v] == (v == V_BASS) && !song.scene_solo[1][v];
        }
        printf("default scenes: F1 the bass alone %d, F2 the breaks alone %d\n", def1, def2);
        song.scene_used[0] = song.scene_used[1] = 0; /* empty, for what follows */
        memset(song.pat.mute, 0, NV);
        memset(song.solo, 0, NV);
        song.pat.mute[1] = song.pat.mute[V_BASS] = 1;
        song.solo[3] = 1;
        uint8_t m1[NV], s1[NV];
        memcpy(m1, song.pat.mute, NV);
        memcpy(s1, song.solo, NV);
        ui_set_page(PAGE_MIX);
        ui_button(BTN_R2, 1), ui_button(BTN_L1, 1), ui_button(BTN_L1, 0), ui_button(BTN_R2, 0);
        int saved = song.scene_used[0] && !memcmp(song.scene_mute[0], m1, NV) && !memcmp(song.scene_solo[0], s1, NV) &&
                    ui_page() == PAGE_MIX; /* R2 + L1 saved, and didn't change the page */
        /* everything playing, then F1: M1's mutes and solos; F1 again: back */
        memset(song.pat.mute, 0, NV);
        memset(song.solo, 0, NV);
        ui_set_page(PAGE_PATTERN);
        int empty_f2 = 0;
        ui_button(BTN_F2, 1), ui_button(BTN_F2, 0); /* M2 empty: nothing changes */
        for (int v = 0; v < NV; v++)
            empty_f2 |= song.pat.mute[v] | song.solo[v];
        empty_f2 = !empty_f2 && ui_scene_on() < 0;
        ui_button(BTN_F1, 1), ui_button(BTN_F1, 0);
        int on = ui_scene_on() == 0 && !memcmp(song.pat.mute, m1, NV) && !memcmp(song.solo, s1, NV);
        ui_button(BTN_F1, 1), ui_button(BTN_F1, 0);
        int off = ui_scene_on() < 0;
        for (int v = 0; v < NV; v++)
            off &= !song.pat.mute[v] && !song.solo[v];
        /* R2 + F2 saves M2 (the bass alone muted); R2 + L1 off MIX switches M1 */
        song.pat.mute[V_BASS] = 1;
        ui_button(BTN_R2, 1), ui_button(BTN_F2, 1), ui_button(BTN_F2, 0), ui_button(BTN_R2, 0);
        song.pat.mute[V_BASS] = 0;
        ui_button(BTN_R2, 1), ui_button(BTN_L1, 1), ui_button(BTN_L1, 0), ui_button(BTN_R2, 0);
        int combo = ui_scene_on() == 0 && ui_page() == PAGE_PATTERN && !memcmp(song.pat.mute, m1, NV);
        /* M2 while M1 is on swaps to it; off from there goes back to before M1 */
        ui_button(BTN_F2, 1), ui_button(BTN_F2, 0);
        int swap = ui_scene_on() == 1 && song.pat.mute[V_BASS] && !song.pat.mute[1] && !song.solo[3];
        ui_button(BTN_F2, 1), ui_button(BTN_F2, 0);
        int back = ui_scene_on() < 0 && !song.pat.mute[V_BASS] && !song.pat.mute[1];
        /* kept in the song file */
        char sp[600];
        snprintf(sp, sizeof sp, "%s/scenes.sav", out);
        song_save(sp);
        Song keep = song;
        song_default();
        int kept = song_load(sp) && song.scene_used[0] && song.scene_used[1] &&
                   !memcmp(song.scene_mute, keep.scene_mute, sizeof keep.scene_mute) &&
                   !memcmp(song.scene_solo, keep.scene_solo, sizeof keep.scene_solo);
        /* a screenshot: MIX with M1 on (lit in the header) and M2 saved (dim) */
        ui_set_page(PAGE_MIX);
        ui_button(BTN_F1, 1), ui_button(BTN_F1, 0);
        ui_draw(fb);
        snprintf(path, sizeof path, "%s/scenes.ppm", out);
        write_ppm(path, fb);
        ui_button(BTN_F1, 1), ui_button(BTN_F1, 0);
        printf("scenes: saved %d, empty does nothing %d, on %d, off %d, R2+L1 %d, swap %d, back %d, kept %d\n", saved,
               empty_f2, on, off, combo, swap, back, kept);
    }

    /* ---- a save from before the scenes (v0.3) still loads, with the default scenes */
    {
        song_default();
        song.bpm = 147.f;
        song.scene_used[0] = 1;
        char p3[600];
        snprintf(p3, sizeof p3, "%s/v03.sav", out);
        song_save(p3);
        FILE *f = fopen(p3, "rb");
        static unsigned char buf[sizeof(Song) + 4096];
        size_t got = fread(buf, 1, sizeof buf, f);
        fclose(f);
        uint32_t v3 = (uint32_t)offsetof(Song, scene_mute);
        memcpy(buf + 4, &v3, 4);
        memmove(buf + v3, buf + sizeof(Song), got - sizeof(Song)); /* drop the new fields */
        f = fopen(p3, "wb");
        fwrite(buf, 1, got - (sizeof(Song) - v3), f);
        fclose(f);
        song_default();
        int ok = song_load(p3);
        printf("save compat v0.3: %d, bpm %.0f, scenes %d\n", ok, song.bpm, song.scene_used[0] + song.scene_used[1]);
    }

    /* ---- the LEDs, into a stand-in for the led_anim driver: F1 lit with M1
     * on, the top bar orange on the bar's first beat, lr in the loudest
     * channel's colour; the settings found at start back at the end */
    {
        char ld[600], fp[700];
        snprintf(ld, sizeof ld, "%s/led_anim", out);
        mkdir(ld, 0755);
        static const char *const files[] = {"effect_rgb_hex_f1", "effect_rgb_hex_f2", "effect_rgb_hex_m",
                                            "effect_rgb_hex_lr", "effect_f1", "effect_f2", "effect_m", "effect_lr",
                                            "max_scale", "max_scale_lr", "max_scale_f1f2", "effect_enable"};
        static const char *const before[] = {"123456", "123456", "123456", "123456", "2", "2", "2", "2",
                                             "10", "10", "10", "1"};
        for (int i = 0; i < 12; i++) {
            snprintf(fp, sizeof fp, "%s/%s", ld, files[i]);
            FILE *f = fopen(fp, "w");
            fprintf(f, "%s\n", before[i]);
            fclose(f);
        }
        setenv("CS_LED_DIR", ld, 1);
        int started = leds_start();
        song_default();
        live.playing = 1;
        live.step = 0;
        for (int v = 0; v <= NV; v++)
            live.meter[v] = 0.f;
        live.meter[2] = 0.6f, live.meter[NV] = 0.5f;
        leds_update(0);
        usleep(80000); /* the LED thread writes it */
        char f1[16], m[16], lr[16], f2[16];
        snprintf(fp, sizeof fp, "%s/effect_rgb_hex_f1", ld);
        FILE *f = fopen(fp, "r");
        int r = fscanf(f, "%15s", f1);
        fclose(f);
        snprintf(fp, sizeof fp, "%s/effect_rgb_hex_f2", ld);
        f = fopen(fp, "r");
        r += fscanf(f, "%15s", f2);
        fclose(f);
        snprintf(fp, sizeof fp, "%s/effect_rgb_hex_m", ld);
        f = fopen(fp, "r");
        r += fscanf(f, "%15s", m);
        fclose(f);
        snprintf(fp, sizeof fp, "%s/effect_rgb_hex_lr", ld);
        f = fopen(fp, "r");
        r += fscanf(f, "%15s", lr);
        fclose(f);
        int lit = r == 4 && !strcmp(f1, "FF2030") && !strcmp(f2, "000000") && !strcmp(m, "FF6414");
        int level = !strcmp(lr, "388957"); /* 0x5adc8c at 5/8 */
        leds_stop();
        live.playing = 0;
        snprintf(fp, sizeof fp, "%s/effect_rgb_hex_f1", ld);
        f = fopen(fp, "r");
        r = fscanf(f, "%15s", f1);
        fclose(f);
        snprintf(fp, sizeof fp, "%s/max_scale_f1f2", ld);
        f = fopen(fp, "r");
        int sc = 0;
        r += fscanf(f, "%d", &sc);
        fclose(f);
        int restored = r == 2 && !strcmp(f1, "123456") && sc == 10;
        unsetenv("CS_LED_DIR");
        printf("leds: driven %d, F1 lit, F2 dark, bar orange %d, level in the loudest channel's colour %d (%s), restored %d\n",
               started, lit, level, lr, restored);
    }

    /* ---- the Brick Pro, LED by LED (frame_hex): F1 lit with M1 on, the top bar's left half lit by a full-scale
     * left channel (the right silent), the shoulders the beat, the left ring's upper corners with the left stick
     * up, the right ring's right corners in REPEAT 1/16's colour with the right stick right; given back at the end */
    {
        char ld[600], fp[700];
        snprintf(ld, sizeof ld, "%s/led_frame", out);
        mkdir(ld, 0755);
        static const char *const files[] = {"effect_rgb_hex_f1", "effect_rgb_hex_f2", "effect_rgb_hex_m",
                                            "effect_rgb_hex_lr", "effect_f1", "effect_f2", "effect_m", "effect_lr",
                                            "max_scale", "max_scale_lr", "max_scale_f1f2", "effect_enable", "frame_hex"};
        static const char *const before[] = {"123456", "123456", "123456", "123456", "2", "2", "2", "2",
                                             "10", "10", "10", "1", "000000"};
        for (int i = 0; i < 13; i++) {
            snprintf(fp, sizeof fp, "%s/%s", ld, files[i]);
            FILE *f = fopen(fp, "w");
            fprintf(f, "%s\n", before[i]);
            fclose(f);
        }
        setenv("CS_LED_DIR", ld, 1);
        setenv("CS_LED_FRAME", "1", 1);
        int started = leds_start();
        song_default();
        live.playing = 1;
        live.step = 0;
        static short pk[256 * 2];
        for (int i = 0; i < 256; i++)
            pk[i * 2] = 32000, pk[i * 2 + 1] = 0;
        leds_audio(pk, 256);
        leds_sticks(0.f, 1.f, 1.f, 0.f);
        leds_update(0);
        usleep(80000); /* the LED thread writes it */
        char frame[400] = "", en[16] = "";
        snprintf(fp, sizeof fp, "%s/frame_hex", ld);
        FILE *f = fopen(fp, "r");
        if (f) {
            if (!fgets(frame, sizeof frame, f))
                frame[0] = 0;
            fclose(f);
        }
        snprintf(fp, sizeof fp, "%s/effect_enable", ld);
        if ((f = fopen(fp, "r"))) {
            if (!fgets(en, sizeof en, f))
                en[0] = 0;
            fclose(f);
        }
        unsigned v[23] = {0};
        int n = 0;
        for (char *q = frame; n < 23 && sscanf(q, "%6x", &v[n]) == 1; q += 7)
            n++;
        int ok = started && n == 23 && en[0] == '0' && v[9] == 0xff2030 && !v[8]; /* F1 lit, F2 dark */
        for (int k = 0; k < 4; k++)
            ok &= v[k] != 0 && v[4 + k] == 0 && v[18 + k] != 0; /* the bar's left half, the shoulders */
        ok &= v[10] && !v[11] && !v[12] && v[13];                 /* the left ring: upper corners */
        ok &= !v[14] && !v[15] && v[16] == 0xffb428 && v[17] == 0xffb428; /* the right ring: right corners */
        ok &= !v[22];
        leds_stop();
        live.playing = 0;
        unsetenv("CS_LED_FRAME");
        unsetenv("CS_LED_DIR");
        leds_sticks(0.f, 0.f, 0.f, 0.f);
        snprintf(fp, sizeof fp, "%s/frame_hex", ld);
        if ((f = fopen(fp, "r"))) {
            if (!fgets(frame, sizeof frame, f))
                frame[0] = 0;
            fclose(f);
        }
        snprintf(fp, sizeof fp, "%s/effect_enable", ld);
        if ((f = fopen(fp, "r"))) {
            if (!fgets(en, sizeof en, f))
                en[0] = 0;
            fclose(f);
        }
        char f1[16] = "";
        snprintf(fp, sizeof fp, "%s/effect_rgb_hex_f1", ld);
        if ((f = fopen(fp, "r"))) {
            if (fscanf(f, "%15s", f1) != 1)
                f1[0] = 0;
            fclose(f);
        }
        int back = !strncmp(frame, "000000 000000", 13) && en[0] == '1' && !strcmp(f1, "123456");
        printf("leds frame: F1, the level on the bar, the beat on the shoulders, each ring its stick: %d; given back "
               "(dark frame, effects on, colours as found): %d\n", ok, back);
    }

    /* ---- sync: a leader's packet sets our tempo */
    {
        song_default();
        song.sync_mode = SYNC_AUTO;
        sync_core_init(100);
        uint8_t pkt[SYNC_PKT_LEN];
        float save_bpm = song.bpm;
        song.bpm = 140.f;
        song.sync_mode = SYNC_LEAD;
        sync_core_init(5);
        sync_make_packet(pkt, 1000);
        song.bpm = save_bpm;
        song.sync_mode = SYNC_AUTO;
        sync_core_init(100);
        sync_handle_packet(pkt, sizeof pkt, 2000);
        printf("sync: bpm=%.1f following=%d\n", song.bpm, sync_status()->have_leader);
    }

    /* ---- sync timing over a slow, uneven network */
    {
        double mean, worst;
        float delay;
        sim_sync(1, &mean, &worst, &delay);
        printf("sync timing: packets 3-43 ms late, read every 33 ms -> off by %.2f ms on average, %.2f at worst, "
               "trip measured %.1f ms (want under 1 and 2: %d)\n",
               mean, worst, delay, mean < 1.0 && worst < 2.0);
        sim_sync(0, &mean, &worst, &delay);
        printf("sync timing, a leader without echo: off by %.2f ms on average (want under 6: %d)\n", mean, mean < 6.0);
    }

    /* ---- the update prompt: A starts the install; the B that closes it doesn't roll a rhythm */
    {
        song_default();
        ui_set_page(PAGE_PATTERN);
        snprintf(fake_update.latest, sizeof fake_update.latest, "9.9.9");
        fake_update.state = UP_AVAILABLE;
        ui_set_update(&fake_update, fake_install);
        ui_tick(10);
        ui_draw(fb);
        int shown = (fb[120 * SCREEN_W + SCREEN_W / 2] & 0xffffff) == 0xff5a2a; /* the panel's orange border */
        snprintf(path, sizeof path, "%s/update.ppm", out);
        write_ppm(path, fb);
        ui_button(BTN_A, 1);
        ui_button(BTN_A, 0);
        ui_draw(fb);
        snprintf(path, sizeof path, "%s/update_install.ppm", out);
        write_ppm(path, fb);
        int started = fake_update.state == UP_INSTALLING;
        fake_update.state = UP_AVAILABLE;
        Pattern before = song.pat;
        ui_button(BTN_B, 1);
        ui_button(BTN_B, 0);
        ui_tick(10);
        ui_draw(fb);
        int closed = (fb[120 * SCREEN_W + SCREEN_W / 2] & 0xffffff) != 0xff5a2a;
        int same = !memcmp(&before, &song.pat, sizeof before);
        printf("update prompt: shown %d, A installs %d, B closes it %d, pattern untouched %d\n", shown, started, closed,
               same);
        ui_set_update(NULL, NULL);
    }
    /* ---- USB MIDI (midi.c), bytes in as from a port: START plays from the
     * first step, the tempo and the phase follow the clock, STOP stops */
    {
        static int16_t pcm[2048 * 2];
        song_default();
        engine_init(RATE);
        engine_set_playing(0);
        midi_reset();
        midi_set_latency(0.f);
        const uint8_t start = 0xfa, stop = 0xfc, tick = 0xf8;
        uint64_t t = 1000000;
        uint32_t trig0 = live.trig_count[0] + live.trig_count[1] + live.trig_count[2] + live.trig_count[3];
        midi_feed(&start, 1, t);
        int started = live.playing;
        engine_render(pcm, 64);
        int first = live.step == 0 &&
                    live.trig_count[0] + live.trig_count[1] + live.trig_count[2] + live.trig_count[3] > trig0;
        double worst = 0.0;
        for (int i = 1; i <= 24 * 16; i++) { /* 16 beats at 120 BPM: a tick each 20.833 ms */
            t += 20833;
            midi_feed(&tick, 1, t);
            engine_render(pcm, 1000); /* the time between ticks */
            if (i > 24 * 12) {
                double err = fabs(engine_position() - i / 6.0);
                worst = err > worst ? err : worst;
            }
        }
        float bpm_in = song.bpm;
        int following = midi_following();
        midi_feed(&stop, 1, t);
        printf("midi clock in: START plays %d from the first step %d, 120 BPM -> %.1f (%d), "
               "phase within %.3f steps (want under 0.1: %d), STOP stops %d\n",
               started, first, bpm_in, fabsf(bpm_in - 120.f) < 0.5f && following, worst, worst < 0.1, !live.playing);

        { /* MIDI CC: the bass chosen, its cutoff, its HIT cell, the tempo, a mute */
            song_default();
            midi_reset();
            uint8_t m[3] = {0xb9, 2, 127}; /* channel 10: any channel is heard */
            int mch, mcc, mv, n = 0;
            midi_feed(m, 3, 1000);
            m[1] = 14 + S_CUT, m[2] = 0;
            midi_feed(m, 3, 2000);
            m[1] = 103, m[2] = 127; /* HIT steps: 32 */
            midi_feed(m, 3, 3000);
            m[1] = 85, m[2] = 127;  /* BPM: 250 */
            midi_feed(m, 3, 4000);
            m[1] = 30, m[2] = 127;  /* muted */
            midi_feed(m, 3, 5000);
            m[1] = 7, m[2] = 100;   /* not one of ours */
            midi_feed(m, 3, 6000);
            while (midi_take_cc(&mch, &mcc, &mv))
                n += ui_midi_cc(mch, mcc, mv);
            int ok = n == 5 && song.pat.param[V_BASS][S_CUT] == 0 && song.pat.param[0][S_CUT] != 0 &&
                     song.pat.cell[V_BASS][ROW_HIT].steps == MAX_STEPS && song.bpm == 250.f &&
                     song.pat.mute[V_BASS] && !song.pat.mute[0];
            printf("midi cc: bass chosen, its cutoff 0, HIT 32 steps, BPM 250, muted, CC7 not ours: %d\n", ok);
            song_default();
            midi_reset();
        }

        { /* MIDI notes: channel 1 plays break 1's slices, channel 5 the bass at the key's pitch */
            song_default();
            engine_init(RATE);
            engine_set_playing(0);
            midi_reset();
            static int16_t nb[512 * 2];
            uint32_t t0 = live.trig_count[0], t2 = live.trig_count[2], tb = live.trig_count[V_BASS];
            uint8_t on1[3] = {0x90, 37, 100}, on5[3] = {0x94, 45, 100}, off5[3] = {0x84, 45, 0};
            midi_feed(on1, 3, 1000);
            midi_feed(on5, 3, 2000);
            engine_render(nb, 512);
            float peak = 0.f;
            for (int k = 0; k < 40; k++) { /* the bass sounds while held */
                engine_render(nb, 512);
                for (int i = 0; i < 1024; i++)
                    peak = fmaxf(peak, fabsf(nb[i] / 32768.f));
            }
            int brk_ok = live.trig_count[0] == t0 + 1 && live.trig_count[2] == t2;
            int bass_ok = live.trig_count[V_BASS] == tb + 1 && live.note == 45 && peak > 0.01f;
            midi_feed(off5, 3, 3000);
            float tail = 0.f;
            for (int k = 0; k < 200; k++)
                engine_render(nb, 512);
            for (int i = 0; i < 1024; i++)
                tail = fmaxf(tail, fabsf(nb[i] / 32768.f));
            printf("midi notes: channel 1 plays break 1 %d, channel 5 the bass at A2 %d (peak %.2f), "
                   "let go fades %d (%.4f)\n", brk_ok, bass_ok, peak, tail < 0.01f, tail);
            song_default();
            midi_reset();
        }

        song_default();
        song.bpm = 120.f;
        engine_init(RATE);
        engine_set_playing(1);
        midi_reset();
        midi_set_latency(0.f);
        uint8_t out[64];
        int ticks_out = 0, starts = 0, stops = 0;
        t = 5000000;
        for (int i = 0; i < 400; i++) { /* 4 s in 10 ms steps */
            engine_render(pcm, RATE / 100);
            t += 10000;
            int n = midi_clock_step(t, out, sizeof out);
            for (int k = 0; k < n; k++)
                ticks_out += out[k] == 0xf8, starts += out[k] == 0xfa;
        }
        engine_set_playing(0);
        int n = midi_clock_step(t + 10000, out, sizeof out);
        for (int k = 0; k < n; k++)
            stops += out[k] == 0xfc;
        printf("midi clock out: START %d, %d ticks in 4 s at 120 BPM (want 192: %d), STOP %d\n", starts, ticks_out,
               abs(ticks_out - 192) <= 1, stops);
        midi_reset();
    }

    { /* JUMP: a click in a clean tone is found; a tone, a square wave, a crushed tone and the beat itself are not */
        static int16_t b[48000 * 2];
        unsigned seg[5];
        for (int k = 0; k < 4; k++) {
            for (int i = 0; i < 48000; i++) {
                double ph = 2 * M_PI * 220.0 * i / 48000.0, x = k == 2 ? (sin(ph) > 0 ? 0.5 : -0.5) : 0.4 * sin(ph);
                if (k == 3)
                    x = floor(x * 8.0) / 8.0;
                if (k == 1 && i == 30000)
                    x += 0.5;
                b[i * 2] = b[i * 2 + 1] = (int16_t)(x * 32767);
            }
            if (k >= 2)
                perf_audio_output(b, 48000); /* a second of it first: the switch to it is a jump of its own */
            unsigned before = perf_jumps();
            perf_audio_output(b, 48000);
            seg[k] = perf_jumps() - before;
        }
        song_default();
        song.bpm = 160.f;
        engine_init(RATE);
        engine_set_playing(1);
        for (int i = 0; i < 4; i++) /* settle */
            engine_render(b, 512), perf_audio_output(b, 512);
        unsigned before = perf_jumps();
        static int16_t beat[20 * RATE * 2];
        for (int i = 0; i < 20 * RATE / 512; i++) {
            engine_render(b, 512);
            perf_audio_output(b, 512);
            memcpy(beat + i * 1024, b, 1024 * sizeof b[0]);
        }
        seg[4] = perf_jumps() - before;
        engine_set_playing(0);
        snprintf(path, sizeof path, "%s/jumpbeat.wav", out);
        write_wav(path, beat, 20 * RATE / 512 * 512);
        printf("perflog jumps: tone %u, tone with a click %u, square %u, crushed %u, 20 s of the beat %u "
               "(want 0 1 0 0 0: %d)\n", seg[0], seg[1], seg[2], seg[3], seg[4],
               seg[0] == 0 && seg[1] == 1 && seg[2] == 0 && seg[3] == 0 && seg[4] == 0);
    }

    /* ---- the Brick Pro's sticks: the left closes the bus filter by hand and
     * lets it go again; the right plays the punch-in effects, R2's d-pad first */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        static float buf[RATE];
        double hf[3];
        for (int k = 0; k < 3; k++) {
            live.sweep_hand = k == 1 ? 1.f : 0.f;
            render_mono(buf, RATE / 2); /* settle */
            render_mono(buf, RATE);
            double e = 0.0;
            for (int i = 1; i < RATE; i++)
                e += (buf[i] - buf[i - 1]) * (double)(buf[i] - buf[i - 1]);
            hf[k] = e;
        }
        live.sweep_hand = 0.f;
        int fx_stick, fx_r2, fx_back, fx_off;
        ui_stick_fx(FX_REVERSE);
        fx_stick = live.fx == FX_REVERSE;
        ui_button(BTN_R2, 1);
        ui_button(BTN_UP, 1);
        fx_r2 = live.fx == FX_REPEAT8;
        ui_button(BTN_UP, 0);
        ui_button(BTN_R2, 0);
        fx_back = live.fx == FX_REVERSE;
        ui_stick_fx(-1);
        fx_off = live.fx == -1;
        engine_set_playing(0);
        printf("stick: filter closed by hand %.1f%% of the highs (darker: %d), let go %.0f%% (open again: %d); "
               "fx: stick %d, R2 first %d, back %d, off %d\n",
               100.0 * hf[1] / hf[0], hf[1] < 0.2 * hf[0], 100.0 * hf[2] / hf[0], hf[2] > 0.5 * hf[0], fx_stick, fx_r2,
               fx_back, fx_off);
    }
    /* ---- the groove (the side switch up): the lock pulls each channel's HIT
     * cell to a traditional rhythm, so at full every bar is the same; part
     * way it changes some steps; ghost notes land on empty 16ths off the beat
     * near a hit (only the e and a of the beat, next to it, at 0.4), quiet, and
     * more of them in the fill at the end of each fourth bar; and at rest
     * nothing changes (session.wav is the same sound) */
    {
        song_default();
        engine_init(RATE);
        int same_full = 1, active = 0, changed_part = 0, changed_full = 0, steps = 0, clave_differs = 0;
        for (int v = 0; v < NV; v++) {
            const Cell *c = &song.pat.cell[v][ROW_HIT];
            if (!c->on || c->hits < 1)
                continue;
            active++;
            for (int s = 0; s < 64; s++) {
                live.groove_lock = 0.f;
                int base = groove_flags(v, s) != 0;
                live.groove_lock = 0.3f;
                changed_part += (groove_flags(v, s) != 0) != base;
                live.groove_lock = 1.f;
                int full = groove_flags(v, s) != 0;
                changed_full += full != base;
                live.groove_lock = -1.f;
                clave_differs += (groove_flags(v, s) != 0) != full;
                live.groove_lock = 1.f;
                if (s >= 16 && full != (groove_flags(v, s - 16) != 0))
                    same_full = 0;
                steps++;
            }
        }
        live.groove_lock = 0.f;
        int ghosts = 0, ghost_bad = 0, low_bad = 0, vel_bad = 0, fill1 = 0, fill4 = 0;
        for (int dir = -1; dir <= 1; dir += 2)
            for (int lvl = 0; lvl < 2; lvl++) {
                live.groove_ghost = dir * (lvl ? 1.f : 0.4f);
                for (int v = 0; v < NBRK; v++)
                    for (int s = 0; s < 256; s++) {
                        float gv = groove_ghost_at(v, s);
                        if (gv <= 0.f)
                            continue;
                        int i = s & 15, by = 0;
                        for (int d = 1; d <= 1 + lvl; d++) {
                            int n = s + (dir > 0 ? -d : d);
                            by |= n >= 0 && groove_flags(v, n) != 0;
                        }
                        ghosts++;
                        ghost_bad += groove_flags(v, s) != 0 || (i & 3) == 0 || !by;
                        low_bad += !lvl && (i & 1) == 0;
                        vel_bad += gv < 0.2f || gv > 0.5f;
                        if (i >= 8 && (s >> 4 & 3) == 0)
                            fill1++;
                        if (i >= 8 && (s >> 4 & 3) == 3)
                            fill4++;
                    }
            }
        /* and they sound: more hits in 4 s with the ghosts than without */
        uint32_t hits[2];
        for (int k = 0; k < 2; k++) {
            engine_init(RATE);
            live.groove_ghost = k ? 1.f : 0.f;
            uint32_t t0 = 0;
            for (int v = 0; v < NBRK; v++)
                t0 += live.trig_count[v];
            engine_set_playing(1);
            static float buf[RATE];
            for (int i = 0; i < 4; i++)
                render_mono(buf, RATE);
            engine_set_playing(0);
            hits[k] = 0;
            for (int v = 0; v < NBRK; v++)
                hits[k] += live.trig_count[v];
            hits[k] -= t0;
        }
        live.groove_ghost = 0.f;
        ui_left_mode(1, "LEFT STICK: GROOVE");
        ui_draw(fb);
        char path[512];
        snprintf(path, sizeof path, "%s/groove.ppm", out);
        write_ppm(path, fb);
        ui_left_mode(0, NULL);
        printf("groove: %d channels, %d steps; lock 0.3 changes %d, full %d, bars repeat at full %d, clave differs %d; "
               "ghosts %d, on an empty 16th off the beat by a hit %d, odd 16ths only at 0.4 %d, quiet %d, "
               "bar 4 fills %d -> %d, heard %u -> %u (want 1 1 1: %d)\n",
               active, steps, changed_part, changed_full, same_full, clave_differs, ghosts, !ghost_bad, !low_bad,
               !vel_bad, fill1, fill4, hits[0], hits[1],
               same_full && changed_part > 0 && changed_part < changed_full && clave_differs > 0 && ghosts > 0 &&
                   !ghost_bad && !low_bad && !vel_bad && fill4 > fill1 && hits[1] > hits[0]);
    }
    /* ---- half time (HOME on the Brick Pro, the side switch on the Brick): halves the tempo, the next press
     * puts back the tempo it was; saves keep the full tempo */
    {
        song_default();
        song.bpm = 163.5f;
        ui_half_time();
        float half = song.bpm, saved = ui_full_bpm();
        ui_draw(fb);
        char path[512];
        snprintf(path, sizeof path, "%s/halftime.ppm", out);
        write_ppm(path, fb);
        ui_half_time();
        float back = song.bpm;
        /* a tempo changed in half time doubles when it ends */
        ui_half_time();
        song.bpm = 90.f;
        ui_half_time();
        float changed = song.bpm;
        /* the Brick's side switch sets it, and setting it twice changes nothing */
        song.bpm = 170.f;
        int set_ok = ui_set_half_time(1) && ui_set_half_time(1) && song.bpm == 85.f && ui_set_half_time(0) &&
                     ui_set_half_time(0) && song.bpm == 170.f;
        printf("half time: 163.5 -> %.1f, saves %.1f, back %.1f; changed to 90 in half time -> %.1f; switch on, on, "
               "off, off from 170: %d (want 1: %d)\n",
               half, saved, back, changed, set_ok,
               half == 81.8f && saved == 163.5f && back == 163.5f && changed == 180.f && set_ok);
    }
    /* ---- the Brick's L2: alone it does nothing; with the d-pad it closes the
     * filter as the left stick does, with A B X Y it plays the right stick's
     * effects (X up, A right, Y left, B down); the page sees none of it */
    {
        song_default();
        ui_set_page(PAGE_PATTERN);
        ui_l2_sticks(1);
        Pattern before = song.pat;
        ui_button(BTN_L2, 1);
        int alone = !live.filter_held && live.fx < 0;
        ui_button(BTN_DOWN, 1);
        int sweep = live.filter_held;
        ui_button(BTN_DOWN, 0);
        int open = !live.filter_held;
        ui_button(BTN_X, 1);
        int fx_x = live.fx == FX_REPEAT8;
        ui_button(BTN_A, 1);
        int fx_a = live.fx == FX_REPEAT16; /* the latest pressed wins */
        ui_button(BTN_A, 0);
        int fx_back = live.fx == FX_REPEAT8;
        ui_button(BTN_X, 0);
        ui_button(BTN_Y, 1);
        int fx_y = live.fx == FX_REVERSE;
        ui_button(BTN_Y, 0);
        ui_button(BTN_B, 1);
        int fx_b = live.fx == FX_TAPESTOP;
        ui_button(BTN_B, 0); /* not a random rhythm on PATTERN */
        ui_button(BTN_LEFT, 1);
        ui_button(BTN_L2, 0); /* let go of L2 first: the filter opens */
        int let_go = !live.filter_held && live.fx < 0;
        ui_button(BTN_LEFT, 0);
        int untouched = !memcmp(&before, &song.pat, sizeof before);
        /* R2 + the d-pad: the groove, not the effects */
        ui_button(BTN_R2, 1);
        ui_button(BTN_UP, 1);
        int g_up = live.groove_lock > 0.f && live.groove_ghost == 0.f && live.fx < 0;
        ui_button(BTN_RIGHT, 1);
        int g_diag = live.groove_lock > 0.f && live.groove_ghost > 0.f;
        ui_button(BTN_UP, 0);
        ui_button(BTN_RIGHT, 0);
        ui_button(BTN_DOWN, 1);
        ui_button(BTN_LEFT, 1);
        int g_down = live.groove_lock < 0.f && live.groove_ghost < 0.f;
        ui_button(BTN_R2, 0); /* let go of R2: the pattern as programmed */
        int g_off = live.groove_lock == 0.f && live.groove_ghost == 0.f;
        ui_button(BTN_DOWN, 0);
        ui_button(BTN_LEFT, 0);
        int g_untouched = !memcmp(&before, &song.pat, sizeof before);
        ui_l2_sticks(0);
        ui_button(BTN_L2, 1);
        int pro = live.filter_held; /* the Brick Pro: L2 alone sweeps, as before */
        ui_button(BTN_L2, 0);
        ui_button(BTN_R2, 1);
        ui_button(BTN_UP, 1);
        int pro_fx = live.fx == FX_REPEAT8 && live.groove_lock == 0.f; /* and R2 + up repeats */
        ui_button(BTN_UP, 0);
        ui_button(BTN_R2, 0);
        printf("brick r2: + up locks %d, + right too %d, down + left %d, let go %d, pattern untouched %d; "
               "Brick Pro R2 + up repeats %d (want 1: %d)\n",
               g_up, g_diag, g_down, g_off, g_untouched, pro_fx, g_up && g_diag && g_down && g_off && g_untouched && pro_fx);
        printf("brick l2: alone %d, + d-pad sweeps %d, opens %d; X %d, A %d, back %d, Y %d, B %d; let go %d; "
               "pattern untouched %d; Brick Pro L2 alone %d (want 1: %d)\n",
               alone, sweep, open, fx_x, fx_a, fx_back, fx_y, fx_b, let_go, untouched, pro,
               alone && sweep && open && fx_x && fx_a && fx_back && fx_y && fx_b && let_go && untouched && pro);
    }
    /* ---- the groove kept with the pattern: R2 + the Brick Pro's left stick
     * changes it (and plays nothing live), it stays when let go, the stick
     * alone plays over it, a slot keeps it, R2 + B lets it go, and on the
     * Brick R2 + A keeps what R2 + the d-pad plays */
    {
        song_default();
        engine_init(RATE);
        ui_l2_sticks(0);
        ui_set_page(PAGE_PATTERN);
        ui_button(BTN_R2, 1);
        ui_left_stick(0.7f, 1.f, 1);
        int edits = live.groove_lock == 0.f && live.groove_ghost == 0.f;
        for (int t = 0; t < 50; t++)
            ui_tick(20);
        int lock1 = song.groove_lock, ghost1 = song.groove_ghost;
        ui_left_stick(0.f, 0.f, 1);
        ui_button(BTN_R2, 0);
        int stays = song.groove_lock == lock1 && song.groove_ghost == ghost1;
        ui_left_stick(0.f, -1.f, 1);
        int over = groove_lock_now() == -1.f && groove_ghost_now() == ghost1 / 100.f;
        ui_left_stick(0.f, 0.f, 1);
        int back = groove_lock_now() == lock1 / 100.f;
        /* kept at 100 the ghosts are those the stick plays all the way over */
        int8_t kl = song.groove_lock, kg = song.groove_ghost;
        song.groove_lock = 0, song.groove_ghost = 100;
        int same = 1;
        for (int v = 0; v < NBRK; v++)
            for (int st = 0; st < 64; st++) {
                float k = groove_ghost_at(v, st);
                live.groove_ghost = 1.f;
                same &= k == groove_ghost_at(v, st);
                live.groove_ghost = 0.f;
            }
        song.groove_lock = kl, song.groove_ghost = kg;
        slot_save(9);
        song.groove_lock = song.groove_ghost = 0;
        slot_load(9);
        int slot_ok = song.groove_lock == lock1 && song.groove_ghost == ghost1 && live.cur_slot == 9;
        ui_draw(fb);
        char gp[600];
        snprintf(gp, sizeof gp, "%s/groove_kept.ppm", out);
        write_ppm(gp, fb);
        ui_button(BTN_R2, 1);
        ui_button(BTN_B, 1);
        ui_button(BTN_B, 0);
        ui_button(BTN_R2, 0);
        int cleared = !song.groove_lock && !song.groove_ghost;
        ui_l2_sticks(1);
        ui_button(BTN_R2, 1);
        ui_button(BTN_UP, 1);
        ui_button(BTN_RIGHT, 1);
        ui_button(BTN_A, 1);
        ui_button(BTN_A, 0);
        ui_button(BTN_UP, 0);
        ui_button(BTN_RIGHT, 0);
        ui_button(BTN_R2, 0);
        int brick = song.groove_lock == 50 && song.groove_ghost == 35 && groove_lock_now() == 0.5f;
        ui_l2_sticks(0);
        song.groove_lock = song.groove_ghost = 0;
        printf("groove kept: R2 + stick edits %d, 1 s up-right -> lock %d ghosts %d, stays %d, the stick plays over %d, "
               "back %d, kept 100 = stick all the way %d, slot J keeps it %d, R2+B lets go %d, Brick R2+A keeps %d "
               "(want 1: %d)\n",
               edits, lock1, ghost1, stays, over, back, same, slot_ok, cleared, brick,
               edits && lock1 >= 70 && lock1 <= 90 && ghost1 >= 30 && ghost1 <= 48 && stays && over && back && same &&
                   slot_ok && cleared && brick);
    }
    /* ---- ten slots: I and J saved and loaded; a v0.4 save (eight slots)
     * loads with I and J empty, switching at the bar; a queued slot switches
     * in on the next beat, half bar, bar, 2 bars or 4 bars */
    {
        song_default();
        engine_init(RATE);
        slot_save(2);
        slot_save(8);
        song.pat.param[0][B_PITCH] = 5;
        song.groove_ghost = -40;
        slot_save(9);
        song.switch_div = SW_2BARS;
        char sp[600];
        snprintf(sp, sizeof sp, "%s/slots.sav", out);
        song_save(sp);
        song_default();
        int ij = song_load(sp) && *song_slot_used(&song, 8) && *song_slot_used(&song, 9) &&
                 song_slot(&song, 9)->param[0][B_PITCH] == 5 && song.slot_groove[9][1] == -40 &&
                 song.switch_div == SW_2BARS;
        FILE *f = fopen(sp, "rb");
        static unsigned char buf[sizeof(Song) + 4096];
        size_t got = fread(buf, 1, sizeof buf, f);
        fclose(f);
        uint32_t v4 = (uint32_t)offsetof(Song, slot_more);
        size_t names8 = (size_t)(1 + NSLOT_V4) * NV * 8;
        memcpy(buf + 4, &v4, 4);
        memmove(buf + v4, buf + sizeof(Song), names8); /* drop the new fields and I's and J's names */
        f = fopen(sp, "wb");
        fwrite(buf, 1, v4 + names8, f);
        fclose(f);
        (void)got;
        song_default();
        int v4ok = song_load(sp) && *song_slot_used(&song, 2) && !*song_slot_used(&song, 8) &&
                   !*song_slot_used(&song, 9) && song.switch_div == SW_BAR && !song.groove_ghost;
        int at[NSWITCH], want_at = 1;
        for (int d = 0; d < NSWITCH; d++) {
            song_default();
            engine_init(RATE);
            slot_save(0);
            song.pat.param[0][B_PITCH] = 3;
            slot_save(1);
            slot_load(0);
            song.switch_div = (uint8_t)d;
            engine_set_playing(1);
            live.step = 0; /* the step before is from the run before */
            static float b2[64];
            while (live.step < 1)
                render_mono(b2, 64);
            slot_load(1);
            at[d] = -1;
            for (int n = 0; n < 200000 && at[d] < 0; n++) {
                render_mono(b2, 64);
                if (live.cur_slot == 1)
                    at[d] = live.step;
            }
            engine_set_playing(0);
            want_at &= at[d] == switch_steps(d);
        }
        printf("slots: %d, I and J saved and loaded %d, v0.4 save %d; switch on the beat %d, 1/2 bar %d, bar %d, "
               "2 bars %d, 4 bars %d (want 1: %d)\n",
               NSLOT, ij, v4ok, at[0], at[1], at[2], at[3], at[4], NSLOT == 10 && ij && v4ok && want_at);
    }
    /* ---- song files from the SONG page: B saves the set (B twice over one
     * already there), A loads it, now when stopped and on the next SWITCH ON
     * while playing; the output and sync settings stay the device's; half
     * time stays on */
    {
        char sd[600], f3[700];
        snprintf(sd, sizeof sd, "%s/songs", out);
        snprintf(f3, sizeof f3, "%s/song03.sav", sd);
        unlink(f3);
        ui_set_song_dir(sd);
        song_default();
        engine_init(RATE);
        ui_set_page(PAGE_SONG);
        ui_button(BTN_UP, 1), ui_button(BTN_UP, 0);       /* from A up to the last row */
        ui_button(BTN_RIGHT, 1), ui_button(BTN_RIGHT, 0); /* SONG */
        for (int k = 0; k < 2; k++)
            ui_button(BTN_X, 1), ui_button(BTN_X, 0); /* song 3 */
        song.bpm = 150.f;
        song.pat.param[1][B_PITCH] = 7;
        slot_save(9);
        song.groove_lock = 30;
        song.switch_div = SW_HALF;
        ui_button(BTN_B, 1), ui_button(BTN_B, 0);
        int saved = access(f3, R_OK) == 0;
        song_default();
        song.out_mode = OUT_MONO_CLOCK;
        song.sync_mode = SYNC_OFF;
        ui_button(BTN_A, 1), ui_button(BTN_A, 0);
        ui_tick(10);
        int loaded = song.bpm == 150.f && song.pat.param[1][B_PITCH] == 7 && *song_slot_used(&song, 9) &&
                     song.groove_lock == 30 && song.switch_div == SW_HALF;
        int device = song.out_mode == OUT_MONO_CLOCK && song.sync_mode == SYNC_OFF;
        song.bpm = 120.f;
        ui_button(BTN_B, 1), ui_button(BTN_B, 0); /* asks */
        song.bpm = 99.f;
        ui_button(BTN_A, 1), ui_button(BTN_A, 0);
        int asks = song.bpm == 150.f;
        song.bpm = 120.f;
        for (int k = 0; k < 2; k++)
            ui_button(BTN_B, 1), ui_button(BTN_B, 0);
        song.bpm = 99.f;
        ui_button(BTN_A, 1), ui_button(BTN_A, 0);
        int over = song.bpm == 120.f;
        /* while playing: in on the next half bar */
        song.pat.param[1][B_PITCH] = 0;
        engine_set_playing(1);
        live.step = 0;
        static float b2[64];
        while (live.step < 1)
            render_mono(b2, 64);
        ui_button(BTN_A, 1), ui_button(BTN_A, 0);
        int waits = song.pat.param[1][B_PITCH] == 0 && live.queued_song != NULL;
        ui_draw(fb);
        char sp[600];
        snprintf(sp, sizeof sp, "%s/songs.ppm", out);
        write_ppm(sp, fb);
        int in_at = -1;
        for (int n = 0; n < 200000 && in_at < 0; n++) {
            render_mono(b2, 64);
            if (!live.queued_song)
                in_at = live.step;
        }
        int switched = waits && in_at == 8 && song.pat.param[1][B_PITCH] == 7;
        engine_set_playing(0);
        ui_tick(10);
        /* half time stays on, at half the song's tempo */
        ui_half_time();
        ui_button(BTN_A, 1), ui_button(BTN_A, 0);
        ui_tick(10);
        int half = song.bpm == 60.f && ui_full_bpm() == 120.f;
        ui_half_time();
        ui_set_page(PAGE_PATTERN);
        printf("song files: saved %d, loaded %d, device settings kept %d, saving over asks first %d then saves %d, "
               "playing switches in at the 1/2 bar %d (step %d), half time kept %d (want 1: %d)\n",
               saved, loaded, device, asks, over, switched, in_at, half,
               saved && loaded && device && asks && over && switched && half);
    }
    /* ---- the one-touch fill: armed in the first half of the bar it plays the
     * second half, a drummer's fill: the kick and snare channels play one of
     * the shapes, hit for hit, while the break running in GRID drops out, and
     * it ends at the bar line by itself; the shapes change, never the same
     * twice running; later in the bar it starts on the next 16th, on the last
     * 16th it waits for the next bar; again calls it off; stopped, nothing;
     * the Brick's L2 tapped alone plays it, L2 + the d-pad doesn't.
     * fill.wav: two bars, the fill in the first */
    {
        static float b2[64];
        int armed = 0, over = 0, plays = 0, grid_out = 0, shape = 0;
        song_default();
        engine_init(RATE);
        engine_set_playing(0);
        engine_set_playing(1);
        live.step = 0;
        while (live.step < 2)
            render_mono(b2, 64);
        ui_fill();
        shape = live.fill_shape;
        armed = live.fill_from == 8 && live.fill_to == 16 && live.fill_snare == 1 && live.fill_kick == 0;
        while (live.step < 7)
            render_mono(b2, 64);
        uint32_t k0 = live.trig_count[0], s0 = live.trig_count[1];
        while (live.step < 15) {
            render_mono(b2, 64);
            if (live.step == 10 && !over) {
                ui_draw(fb);
                char fp[600];
                snprintf(fp, sizeof fp, "%s/fill.ppm", out);
                write_ppm(fp, fb);
                over = 1;
            }
            if (live.step == 13)
                grid_out = live.play_pos[3] < 0.f;
        }
        const char *sh = engine_fill_shape(shape);
        uint32_t want_k = 0, want_s = 0;
        for (int i = 0; i < 8; i++)
            want_k += sh[i] == 'K', want_s += sh[i] != 'K' && sh[i] != '.';
        plays = live.trig_count[0] - k0 == want_k && live.trig_count[1] - s0 == want_s;
        while (live.step < 16)
            render_mono(b2, 64);
        over = live.fill_to == 0;
        /* twelve fills, one a bar */
        int prev = shape, repeats = 0, seen = 1 << shape, kinds = 0;
        for (int n = 0; n < 12; n++) {
            ui_fill();
            repeats += live.fill_shape == prev;
            prev = live.fill_shape;
            seen |= 1 << prev;
            int stop = live.fill_to;
            while (live.step < stop)
                render_mono(b2, 64);
        }
        for (int i = 0; i < engine_fill_shapes(); i++)
            kinds += seen >> i & 1;
        engine_set_playing(0);
        /* later in the bar, the last 16th, again, stopped */
        engine_init(RATE);
        engine_set_playing(0);
        engine_set_playing(1);
        live.step = 0;
        while (live.step < 10)
            render_mono(b2, 64);
        ui_fill();
        int late = live.fill_from == 11 && live.fill_to == 16;
        ui_fill();
        int off = live.fill_to == 0;
        while (live.step < 14)
            render_mono(b2, 64);
        ui_fill();
        int next_bar = live.fill_from == 24 && live.fill_to == 32;
        engine_set_playing(0);
        int stop_clears = live.fill_to == 0;
        ui_fill();
        int stopped = live.fill_to == 0;
        /* the Brick: L2 alone tapped plays it, L2 + the d-pad not */
        ui_l2_sticks(1);
        engine_set_playing(1);
        ui_button(BTN_L2, 1);
        ui_button(BTN_L2, 0);
        int tap = live.fill_to > 0;
        ui_button(BTN_L2, 1);
        ui_button(BTN_L2, 0); /* again: off */
        ui_button(BTN_L2, 1);
        ui_button(BTN_DOWN, 1);
        ui_button(BTN_DOWN, 0);
        ui_button(BTN_L2, 0);
        int combo = live.fill_to == 0;
        engine_set_playing(0);
        ui_l2_sticks(0);
        /* to listen to */
        {
            song_default();
            engine_init(RATE);
            engine_set_playing(0);
            engine_set_playing(1);
            live.step = 0;
            int frames = (int)(2 * 16 * RATE * 15.f / song.bpm) / 512 * 512 + 512;
            int16_t *pcm = malloc(sizeof(int16_t) * 2 * frames);
            engine_render(pcm, 512);
            ui_fill();
            for (int done = 512; done < frames; done += 512)
                engine_render(pcm + 2 * done, 512);
            engine_set_playing(0);
            char wp[600];
            snprintf(wp, sizeof wp, "%s/fill.wav", out);
            write_wav(wp, pcm, frames);
            free(pcm);
        }
        printf("fill: armed for the 2nd half %d, shape %s played hit for hit %d, the GRID break out %d, over at the "
               "bar line %d; 12 more: twice running %d, %d of %d shapes; later from the next 16th %d, again off %d, "
               "last 16th the next bar %d, stop clears %d, stopped nothing %d; Brick L2 tap %d, L2 + d-pad not %d "
               "(want 1: %d)\n",
               armed, sh, plays, grid_out, over, repeats, kinds, engine_fill_shapes(), late, off, next_bar, stop_clears,
               stopped, tap, combo,
               armed && plays && grid_out && over && !repeats && kinds >= 5 && late && off && next_bar && stop_clears &&
                   stopped && tap && combo);
    }
    /* ---- the groove's feel on the SONG page's GROOVE row: SWING delays the
     * 16ths between the 8ths (75%: half a 16th) and leaves the rest on the
     * grid; BREATH softens all but the beats, swells into the fourth bar and
     * leans the off-beat hits a little late; at 0 nothing moves (session.wav);
     * X / Y / A on the row change them; a slot keeps them; R2 + B lets go */
    {
        static float b1[1];
        int delay[16];
        song_default();
        song.swing = 25;
        Cell *c = &song.pat.cell[0][ROW_HIT];
        c->on = 1, c->hits = 16, c->steps = 16, c->rot = 0;
        song.pat.cell[0][ROW_ROLL].on = song.pat.cell[0][ROW_ALT].on = 0;
        engine_init(RATE);
        engine_set_playing(0);
        engine_set_playing(1);
        live.step = -1;
        int at = 0, step_at = 0, cur = -1;
        uint32_t t = live.trig_count[0];
        for (int i = 0; i < 16; i++)
            delay[i] = -1;
        while (cur < 16 && at < RATE * 4) {
            render_mono(b1, 1);
            at++;
            if (live.step != cur)
                cur = live.step, step_at = at;
            if (live.trig_count[0] != t) {
                t = live.trig_count[0];
                if (cur >= 0 && cur < 16 && delay[cur] < 0)
                    delay[cur] = at - step_at;
            }
        }
        engine_set_playing(0);
        float sps = RATE * 15.f / song.bpm;
        int swing_ok = 1;
        for (int i = 0; i < 16; i++)
            swing_ok &= i & 1 ? abs(delay[i] - (int)(0.5f * sps)) <= 2 : delay[i] == 0;
        /* breath */
        song.swing = 0;
        song.breath = 0;
        int flat = 1;
        for (int st = 0; st < 64; st++)
            flat &= engine_step_gain(st) == 1.f;
        song.breath = 100;
        /* in the fourth bar the beat is full, the 8th offbeat softer, the 16th between softer still;
         * the same step in the first bar softer than in the fourth */
        int shape = engine_step_gain(48) == 1.f && engine_step_gain(50) < 1.f &&
                    engine_step_gain(49) < engine_step_gain(50) && engine_step_gain(1) < engine_step_gain(49);
        engine_init(RATE);
        engine_set_playing(1);
        live.step = -1;
        at = 0, cur = -1, t = live.trig_count[0];
        int lean = -1, beat = -1;
        while (cur < 3 && at < RATE) {
            render_mono(b1, 1);
            at++;
            if (live.step != cur)
                cur = live.step, step_at = at;
            if (live.trig_count[0] != t) {
                t = live.trig_count[0];
                if (cur == 0 && beat < 0)
                    beat = at - step_at;
                if (cur == 2 && lean < 0)
                    lean = at - step_at;
            }
        }
        engine_set_playing(0);
        int lean_ok = beat == 0 && lean >= 0 && lean <= (int)(0.06f * sps) + 1;
        song.breath = 0;
        /* the GROOVE row: from SWITCH / SONG up one row, right to SWING */
        ui_set_page(PAGE_SONG);
        ui_button(BTN_UP, 1), ui_button(BTN_UP, 0);
        ui_button(BTN_RIGHT, 1), ui_button(BTN_RIGHT, 0);
        ui_button(BTN_RIGHT, 1), ui_button(BTN_RIGHT, 0);
        ui_button(BTN_X, 1), ui_button(BTN_X, 0);
        int row_swing = song.swing == 1;
        ui_button(BTN_RIGHT, 1), ui_button(BTN_RIGHT, 0);
        for (int k = 0; k < 3; k++)
            ui_button(BTN_X, 1), ui_button(BTN_X, 0);
        int row_breath = song.breath == 3;
        ui_button(BTN_A, 1), ui_button(BTN_A, 0);
        row_breath &= song.breath == 0;
        song.swing = 8, song.breath = 40;
        ui_draw(fb);
        char gp[600];
        snprintf(gp, sizeof gp, "%s/groove_row.ppm", out);
        write_ppm(gp, fb);
        slot_save(4);
        song.swing = song.breath = 0;
        slot_load(4);
        int slot = song.swing == 8 && song.breath == 40;
        ui_button(BTN_R2, 1);
        ui_button(BTN_B, 1), ui_button(BTN_B, 0);
        ui_button(BTN_R2, 0);
        int cleared = !song.swing && !song.breath;
        ui_set_page(PAGE_PATTERN);
        printf("feel: swing 75%% delays the 16ths between the 8ths half a 16th, the rest on the grid %d; breath 0 flat %d, "
               "beats full, offbeats softer, the phrase swelling %d, a lean of %d frames, the beat on time %d; the GROOVE "
               "row: swing %d, breath %d; a slot keeps them %d; R2+B lets go %d (want 1: %d)\n",
               swing_ok, flat, shape, lean, lean_ok, row_swing, row_breath, slot, cleared,
               swing_ok && flat && shape && lean_ok && row_swing && row_breath && slot && cleared);
    }
    /* ---- R2 + R3 (the Brick Pro): every break channel's LENGTH to 20%, the
     * bass untouched, no fill; R3 alone still plays the fill */
    {
        static float b2[64];
        song_default();
        engine_init(RATE);
        int bass_len = song.pat.param[V_BASS][S_LEN];
        engine_set_playing(0);
        engine_set_playing(1);
        live.step = 0;
        while (live.step < 2)
            render_mono(b2, 64);
        ui_button(BTN_R2, 1);
        ui_r3();
        ui_button(BTN_R2, 0);
        int all = 1;
        for (int v = 0; v < NBRK; v++)
            all &= song.pat.param[v][B_LEN] == 20;
        int bass = song.pat.param[V_BASS][S_LEN] == bass_len, no_fill = live.fill_to == 0;
        ui_r3();
        int fill = live.fill_to > 0;
        engine_set_playing(0);
        printf("r2+r3: every break channel's length 20%% %d, the bass untouched %d, no fill %d; R3 alone fills %d "
               "(want 1: %d)\n",
               all, bass, no_fill, fill, all && bass && no_fill && fill);
    }
    breaks_free();
    return 0;
}
