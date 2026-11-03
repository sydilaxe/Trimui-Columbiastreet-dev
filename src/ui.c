#include "ui.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "breaks.h"
#include "engine.h"
#include "gfx.h"
#include "sync.h"
#include "update.h"

#define C_BG 0x0b0c10
#define C_PANEL 0x15171e
#define C_PANEL2 0x20232d
#define C_LINE 0x323644
#define C_TEXT 0xeceef4
#define C_DIM 0x6d7286
#define C_ACCENT 0xff5a2a /* brick orange */
#define C_PLAY 0x3cdc78
#define C_MUTE 0xff3c50
#define C_SOLO 0xffd23c

#define HEADER_H 44
#define BODY_Y 58
#define FOOTER_Y 452

static void nop(void) {}
void (*ui_lock)(void) = nop;
void (*ui_unlock)(void) = nop;

static const uint32_t vcol[NV] = {0xff5a46, 0xffb428, 0x5adc8c, 0x46b4ff, 0xb478ff};
static const char *const row_names[NROW] = {"HIT", "ROLL", "ALT"};
static const char *const page_names[PAGE_COUNT] = {"PATTERN", "CHOP", "BASS", "MIX", "SONG"};
static const char *const sync_mode_names[NSYNCMODE] = {"OFF", "AUTO", "LEAD", "FOLLOW"};

static int page;
static int pat_v, pat_r, chop_v, chop_p, bass_p, mix_v, mix_r, song_cur;
static int held[BTN_COUNT], hold_ms[BTN_COUNT], rep_ms[BTN_COUNT], rep_n[BTN_COUNT];
static int b_used; /* B was a modifier this press (PATTERN page) */
static int fx_order[4], fx_norder;
static int stick_fx = -1; /* the Brick Pro's right stick, or -1 */
static int half_on;            /* half time: song.bpm is half of half_from */
static float half_from, half_to; /* the tempo before, and the one it became */
static int left_groove = -1; /* the side switch: 1 GROOVE, 0 FILTER, -1 not read */
static uint32_t now_ms;
static uint32_t taps[6];
static int ntaps;
static char toast[64];
static int toast_ms;
static int quit, changed;
static float meter_disp[NV + 1], flash[NV];
static uint32_t seen_trig[NV];
static Pattern undo_pat;
static int have_undo;
static const UpdateStatus *upd;
static void (*upd_install)(void);
static int upd_open, upd_asked; /* the update prompt is up; it has been shown this session */
/* the mute scene switched on (0, 1) or -1, and the mutes and solos it
 * replaced, which switching it off brings back */
static int scene_on = -1;
static uint8_t scene_was_mute[NV], scene_was_solo[NV];

/* mix page rows: voices have LEVEL, PAN, SEND; the master column has more */
enum { MX_LEVEL, MX_PAN, MX_SEND, NMXV };
enum { MM_LEVEL, MM_GLUE, MM_DRIVE, MM_VERB, MM_TONE, MM_SWEEP, NMXM };
static const char *const mxv_names[NMXV] = {"LEVEL", "PAN", "SEND"};
static const char *const mxm_names[NMXM] = {"LEVEL", "GLUE", "DRIVE", "VERB", "TONE", "L2"};

/* song page stops: slots A..H, then TEMPO and SYNC */
#define SG_TEMPO NSLOT
#define SG_SYNC (NSLOT + 1)
#define SG_OUT (NSLOT + 2) /* OUTPUT, CLOCK ON, PPQN, PULSE: left / right picks one */
#define SG_N (NSLOT + 3)
static int out_f;

static void say(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast, sizeof toast, fmt, ap);
    va_end(ap);
    toast_ms = 1400;
}

void ui_init(void)
{
    for (int v = 0; v < NV; v++)
        seen_trig[v] = live.trig_count[v];
}

void ui_set_update(const UpdateStatus *status, void (*install)(void))
{
    upd = status;
    upd_install = install;
}

void ui_notice(const char *msg) { say("%s", msg); }
int ui_quit_requested(void) { return quit; }
int ui_page(void) { return page; }
void ui_set_page(int p) { page = p; }

int ui_take_changed(void)
{
    int c = changed;
    changed = 0;
    return c;
}

static int wrap(int x, int n) { return (x % n + n) % n; }
static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }
static int is_dpad(int b) { return b <= BTN_RIGHT; }

/* ---------------------------------------------------------------- input */

/* R2 + up / right / left / down; the latest direction pressed wins. The
 * Brick Pro's right stick plays them too, when R2 isn't holding one. */
static void update_fx(void)
{
    static const int fx_of[4] = {FX_REPEAT8, FX_TAPESTOP, FX_REVERSE, FX_REPEAT16};
    live.fx = upd_open ? -1 : stick_fx;
    if (!held[BTN_R2])
        return;
    for (int i = fx_norder - 1; i >= 0; i--)
        if (held[fx_order[i]]) {
            live.fx = fx_of[fx_order[i]];
            return;
        }
}

static void note_dpad_press(int b)
{
    int n = 0;
    for (int i = 0; i < fx_norder; i++)
        if (fx_order[i] != b)
            fx_order[n++] = fx_order[i];
    fx_order[n++] = b;
    fx_norder = n;
}

static void tap_tempo(void)
{
    if (ntaps && now_ms - taps[ntaps - 1] > 2000)
        ntaps = 0;
    if (ntaps == 6) {
        memmove(taps, taps + 1, 5 * sizeof taps[0]);
        ntaps = 5;
    }
    taps[ntaps++] = now_ms;
    if (ntaps < 2) {
        say("TAP...");
        return;
    }
    float ms = (float)(taps[ntaps - 1] - taps[0]) / (ntaps - 1);
    float bpm = 60000.f / ms;
    /* jungle lives at double time: taps on the half-time beat count double */
    if (bpm < 100.f)
        bpm *= 2.f;
    bpm = bpm < 40.f ? 40.f : bpm > 250.f ? 250.f : bpm;
    song.bpm = roundf(bpm * 10.f) / 10.f;
    say("TAP  %.1f BPM", song.bpm);
}

void ui_stick_fx(int fx)
{
    ui_lock();
    stick_fx = fx;
    update_fx();
    ui_unlock();
}

void ui_half_time(void)
{
    if (upd_open)
        return;
    ui_lock();
    if (!half_on) {
        half_from = song.bpm;
        song.bpm = fmaxf(40.f, roundf(song.bpm * 5.f) / 10.f);
        half_to = song.bpm;
        half_on = 1;
        say("HALF TIME  %.1f BPM", song.bpm);
    }
    else {
        /* the tempo it was, unless it was changed in half time: then double that */
        song.bpm = song.bpm == half_to ? half_from : fminf(250.f, song.bpm * 2.f);
        half_on = 0;
        say("FULL TIME  %.1f BPM", song.bpm);
    }
    ui_unlock();
}

int ui_set_half_time(int on)
{
    if ((on != 0) != half_on)
        ui_half_time();
    return (on != 0) == half_on;
}

float ui_full_bpm(void)
{
    if (!half_on)
        return song.bpm;
    return song.bpm == half_to ? half_from : fminf(250.f, song.bpm * 2.f);
}

void ui_left_mode(int groove, const char *msg)
{
    left_groove = groove;
    if (msg)
        say("%s", msg);
}

void ui_tap_tempo(void)
{
    if (upd_open)
        return;
    ui_lock();
    tap_tempo();
    changed = 1;
    ui_unlock();
}

static void adjust_param(int v, int p, int dir, int repeats)
{
    int lo = param_min(v, p), hi = param_max(v, p);
    int step = (repeats > 12 && hi - lo > 30) ? 4 : 1;
    int *pp = &song.pat.param[v][p];
    int x = clampi(*pp + dir * step, lo, hi);
    if (x == *pp)
        return;
    param_set(v, p, x);
    if (!live.playing && repeats == 0 && !(v == V_BASS && p == S_LEN))
        engine_audition(v);
}

/* SELECT and its undo change the beat, not the performance: the mutes
 * stay as they are (the mixer lives outside the pattern and is untouched) */
static void keep_mutes(Pattern *p, const uint8_t *mute) { memcpy(p->mute, mute, sizeof p->mute); }

static void generate(void)
{
    uint8_t mute[NV];
    memcpy(mute, song.pat.mute, sizeof mute);
    undo_pat = song.pat;
    have_undo = 1;
    pattern_generate(&song.pat, (unsigned)(now_ms * 2654435761u) ^ (unsigned)rand());
    keep_mutes(&song.pat, mute);
    song_clamp();
    say("NEW BEAT  (R2+SELECT: UNDO)");
}

static void pattern_action(int b)
{
    Cell *c = &song.pat.cell[pat_v][pat_r];
    /* B held is a modifier: up / down for the length, left / right rotate */
    if (held[BTN_B] && is_dpad(b)) {
        b_used = 1;
        switch (b) {
        case BTN_UP: c->steps = (uint8_t)clampi(c->steps + 1, 1, MAX_STEPS); break;
        case BTN_DOWN: c->steps = (uint8_t)clampi(c->steps - 1, 1, MAX_STEPS); break;
        case BTN_LEFT: c->rot = (uint8_t)((c->rot + c->steps - 1) % c->steps); break;
        case BTN_RIGHT: c->rot = (uint8_t)((c->rot + 1) % c->steps); break;
        }
        if (c->hits > c->steps)
            c->hits = c->steps;
        c->rot %= c->steps;
        say("%s %s  E(%d,%d) >%d", voice_names[pat_v], row_names[pat_r], c->hits, c->steps, c->rot);
        return;
    }
    switch (b) {
    case BTN_UP: pat_v = wrap(pat_v - 1, NV); break;
    case BTN_DOWN: pat_v = wrap(pat_v + 1, NV); break;
    case BTN_LEFT: pat_r = wrap(pat_r - 1, NROW); break;
    case BTN_RIGHT: pat_r = wrap(pat_r + 1, NROW); break;
    case BTN_A: c->on = !c->on; break;
    case BTN_X:
    case BTN_Y:
        c->hits = (uint8_t)clampi(c->hits + (b == BTN_X ? 1 : -1), 0, c->steps);
        say("%s %s  %d/%d", voice_names[pat_v], row_names[pat_r], c->hits, c->steps);
        break;
    }
}

static void pattern_b_released(void)
{
    if (b_used)
        return;
    Cell *c = &song.pat.cell[pat_v][pat_r];
    euclid_permute(c);
    say("%s %s  E(%d,%d) >%d", voice_names[pat_v], row_names[pat_r], c->hits, c->steps, c->rot);
}

static void chop_action(int b, int repeats)
{
    switch (b) {
    case BTN_UP: chop_p = wrap(chop_p - 1, NP); break;
    case BTN_DOWN: chop_p = wrap(chop_p + 1, NP); break;
    case BTN_LEFT: chop_v = wrap(chop_v - 1, NBRK); break;
    case BTN_RIGHT: chop_v = wrap(chop_v + 1, NBRK); break;
    case BTN_X: adjust_param(chop_v, chop_p, +1, repeats); break;
    case BTN_Y: adjust_param(chop_v, chop_p, -1, repeats); break;
    case BTN_A: engine_audition(chop_v); break;
    case BTN_B:
        song.pat.mute[chop_v] = !song.pat.mute[chop_v];
        say("%s %s", voice_names[chop_v], song.pat.mute[chop_v] ? "MUTED" : "ON");
        break;
    }
}

static void bass_action(int b, int repeats)
{
    int half = (NBASS_P + 1) / 2;
    switch (b) {
    case BTN_UP: bass_p = wrap(bass_p - 1, NBASS_P); break;
    case BTN_DOWN: bass_p = wrap(bass_p + 1, NBASS_P); break;
    case BTN_LEFT:
    case BTN_RIGHT: bass_p = bass_p < half ? bass_p + half : bass_p - half; break;
    case BTN_X: adjust_param(V_BASS, bass_p, +1, repeats); break;
    case BTN_Y: adjust_param(V_BASS, bass_p, -1, repeats); break;
    case BTN_A: engine_audition(V_BASS); break;
    case BTN_B:
        song.pat.mute[V_BASS] = !song.pat.mute[V_BASS];
        say("BASS %s", song.pat.mute[V_BASS] ? "MUTED" : "ON");
        break;
    }
}

static void mix_adjust(int dir, int repeats)
{
    int st = repeats > 12 ? 4 : 2;
    if (mix_v == NV) {
        switch (mix_r) {
        case MM_LEVEL: song.master = clampi(song.master + st * dir, 0, 100); break;
        case MM_GLUE: song.glue = clampi(song.glue + st * dir, 0, 100); break;
        case MM_DRIVE: song.master_drive = clampi(song.master_drive + st * dir, 0, 100); break;
        case MM_VERB: song.verb_time = clampi(song.verb_time + st * dir, 0, 100); break;
        case MM_TONE: song.verb_tone = clampi(song.verb_tone + st * dir, 0, 100); break;
        case MM_SWEEP:
            if (!repeats)
                song.sweep_rate = wrap(song.sweep_rate + dir, nsweep());
            break;
        }
        return;
    }
    switch (mix_r) {
    case MX_LEVEL: song.vol[mix_v] = clampi(song.vol[mix_v] + st * dir, 0, 100); break;
    case MX_PAN: song.pan[mix_v] = clampi(song.pan[mix_v] + 5 * dir, -100, 100); break;
    case MX_SEND: song.send[mix_v] = clampi(song.send[mix_v] + st * dir, 0, 100); break;
    }
}

static void mix_action(int b, int repeats)
{
    int nrows = mix_v == NV ? NMXM : NMXV;
    switch (b) {
    case BTN_UP: mix_r = wrap(mix_r - 1, nrows); break;
    case BTN_DOWN: mix_r = wrap(mix_r + 1, nrows); break;
    case BTN_LEFT:
    case BTN_RIGHT:
        mix_v = wrap(mix_v + (b == BTN_RIGHT ? 1 : -1), NV + 1);
        if (mix_v < NV && mix_r >= NMXV)
            mix_r = NMXV - 1;
        break;
    case BTN_X: mix_adjust(+1, repeats); break;
    case BTN_Y: mix_adjust(-1, repeats); break;
    case BTN_A:
        if (mix_v < NV)
            song.pat.mute[mix_v] = !song.pat.mute[mix_v];
        break;
    case BTN_B:
        if (mix_v < NV)
            song.solo[mix_v] = !song.solo[mix_v];
        break;
    }
}

static void song_action(int b, int repeats)
{
    if (b == BTN_UP || b == BTN_DOWN) {
        int d = b == BTN_DOWN ? 1 : -1;
        if (song_cur < NSLOT) {
            int r = song_cur / 4 + d;
            song_cur = r < 0 ? SG_N - 1 : r < 2 ? r * 4 + song_cur % 4 : SG_TEMPO;
        }
        else {
            int s = song_cur + d;
            song_cur = s < SG_TEMPO ? 4 : s >= SG_N ? 0 : s;
        }
        return;
    }
    if (b == BTN_LEFT || b == BTN_RIGHT) {
        if (song_cur < NSLOT)
            song_cur = (song_cur / 4) * 4 + wrap(song_cur % 4 + (b == BTN_RIGHT ? 1 : -1), 4);
        else if (song_cur == SG_OUT)
            out_f = wrap(out_f + (b == BTN_RIGHT ? 1 : -1), 4);
        return;
    }
    if (song_cur == SG_TEMPO) {
        if (b == BTN_X || b == BTN_Y) {
            float st = repeats > 12 ? 2.f : repeats ? 1.f : 0.5f;
            song.bpm = fminf(250.f, fmaxf(40.f, song.bpm + (b == BTN_X ? st : -st)));
        }
        return;
    }
    if (song_cur == SG_SYNC) {
        if ((b == BTN_X || b == BTN_Y || b == BTN_A) && !repeats) {
            song.sync_mode = wrap(song.sync_mode + (b == BTN_Y ? -1 : 1), NSYNCMODE);
            engine_set_rate(1.0);
            say("WIFI SYNC  %s", sync_mode_names[song.sync_mode]);
        }
        return;
    }
    if (song_cur == SG_OUT) {
        if (b != BTN_X && b != BTN_Y && b != BTN_A)
            return;
        int d = b == BTN_Y ? -1 : 1;
        if (out_f == 0 && !repeats) {
            song.out_mode = !song.out_mode;
            say("%s", song.out_mode == OUT_MONO_CLOCK ? "MONO AUDIO + CLOCK OUT" : "STEREO OUT");
        }
        else if (out_f == 1 && !repeats) {
            song.clk_side = !song.clk_side;
            say("CLOCK ON THE %s", song.clk_side ? "LEFT" : "RIGHT");
        }
        else if (out_f == 2 && b != BTN_A) {
            song.clk_ppqn = clampi(song.clk_ppqn + d, 0, NPPQN - 1);
            int q = clk_ppqn_value(song.clk_ppqn);
            say("%d PPQN%s", q, q == 2 ? "  POCKET OPERATOR / VOLCA" : q == 24 ? "  DIN SYNC" : "");
        }
        else if (out_f == 3 && b != BTN_A)
            song.clk_width = clampi(song.clk_width + d, 1, 20);
        return;
    }
    int i = song_cur;
    switch (b) {
    case BTN_A:
        if (!song.slot_used[i])
            say("SLOT %c IS EMPTY", 'A' + i);
        else {
            slot_load(i);
            say(live.playing ? "%c AT THE NEXT BAR" : "LOADED %c", 'A' + i);
        }
        break;
    case BTN_X:
        slot_save(i);
        say("SAVED PATTERN TO %c", 'A' + i);
        break;
    case BTN_Y:
        if (held[BTN_R2]) {
            slot_clear(i);
            say("CLEARED %c", 'A' + i);
        }
        else
            say("R2+Y CLEARS %c", 'A' + i);
        break;
    }
}

/* ------- mute scenes
 * Two saved sets of the mixer's mutes and solos (M1, M2), to switch on and
 * off in one press while the beat plays: F1 / F2 on the TrimUI, or R2 + L1 /
 * R1 on any page but MIX. Saving: R2 + F1 / F2, or R2 + L1 / R1 on MIX. Switching
 * one on remembers the mutes and solos it replaces; switching it off (the same
 * button again) brings those back. The other scene while one is on swaps
 * straight to it, and off still goes back to how things were before either. */
static void scene_save(int k)
{
    memcpy(song.scene_mute[k], song.pat.mute, NV);
    memcpy(song.scene_solo[k], song.solo, NV);
    song.scene_used[k] = 1;
    if (scene_on >= 0 && scene_on != k)
        scene_on = -1; /* the mixer as it is now is what the next press switches from */
    say("MUTES SAVED TO M%d", k + 1);
}

static void scene_toggle(int k)
{
    if (!song.scene_used[k]) {
        say("M%d IS EMPTY: R2+%s SAVES THE MUTES", k + 1, page == PAGE_MIX ? (k ? "R1" : "L1") : (k ? "F2" : "F1"));
        return;
    }
    if (scene_on == k) {
        memcpy(song.pat.mute, scene_was_mute, NV);
        memcpy(song.solo, scene_was_solo, NV);
        scene_on = -1;
        say("M%d OFF", k + 1);
        return;
    }
    if (scene_on < 0) {
        memcpy(scene_was_mute, song.pat.mute, NV);
        memcpy(scene_was_solo, song.solo, NV);
    }
    memcpy(song.pat.mute, song.scene_mute[k], NV);
    memcpy(song.solo, song.scene_solo[k], NV);
    scene_on = k;
    say("M%d ON", k + 1);
}

int ui_scene_on(void) { return scene_on; }

static void action(int b, int repeats)
{
    /* the mute scenes, before L1 / R1 change the page */
    if (b == BTN_F1 || b == BTN_F2) {
        int k = b == BTN_F2;
        if (held[BTN_R2])
            scene_save(k);
        else
            scene_toggle(k);
        changed = 1;
        return;
    }
    if (held[BTN_R2] && (b == BTN_L1 || b == BTN_R1)) {
        int k = b == BTN_R1;
        if (page == PAGE_MIX)
            scene_save(k);
        else
            scene_toggle(k);
        changed = 1;
        return;
    }
    if (held[BTN_R2] && is_dpad(b))
        return; /* the d-pad plays the punch-in effects while R2 is held */
    if (held[BTN_R2] && b == BTN_X) {
        tap_tempo();
        changed = 1;
        return;
    }
    switch (b) {
    case BTN_L1: page = wrap(page - 1, PAGE_COUNT); return;
    case BTN_R1: page = wrap(page + 1, PAGE_COUNT); return;
    case BTN_START: engine_set_playing(!live.playing); return;
    case BTN_MENU: quit = 1; return;
    case BTN_SELECT:
        if (held[BTN_R2]) {
            if (have_undo) {
                Pattern t = song.pat;
                song.pat = undo_pat;
                undo_pat = t;
                keep_mutes(&song.pat, t.mute);
                song_clamp();
                say("UNDONE");
            }
        }
        else
            generate();
        changed = 1;
        return;
    }
    changed = 1;
    switch (page) {
    case PAGE_PATTERN: pattern_action(b); break;
    case PAGE_CHOP: chop_action(b, repeats); break;
    case PAGE_BASS: bass_action(b, repeats); break;
    case PAGE_MIX: mix_action(b, repeats); break;
    case PAGE_SONG: song_action(b, repeats); break;
    }
}

/* the update prompt takes the buttons while it is up; the music keeps playing */
static void update_action(int b)
{
    int st = upd->state;
    if (st == UP_INSTALLING || st == UP_INSTALLED)
        return;
    if (b == BTN_A && (st == UP_AVAILABLE || st == UP_FAILED))
        upd_install();
    else if (b == BTN_B || b == BTN_MENU) {
        upd_open = 0;
        if (st == UP_AVAILABLE)
            say("V%s IS OFFERED AGAIN NEXT TIME", upd->latest);
    }
}

static int repeatable(int b)
{
    if (is_dpad(b))
        return !held[BTN_R2];
    if (held[BTN_R2])
        return 0;
    if (b == BTN_X || b == BTN_Y)
        return page != PAGE_SONG || song_cur == SG_TEMPO || (song_cur == SG_OUT && out_f >= 2);
    return 0;
}

void ui_button(int b, int down)
{
    if (b < 0 || b >= BTN_COUNT || held[b] == down)
        return;
    held[b] = down;
    hold_ms[b] = rep_ms[b] = rep_n[b] = 0;
    ui_lock();
    if (b == BTN_L2)
        live.filter_held = down;
    if (down && is_dpad(b) && !upd_open)
        note_dpad_press(b);
    if (upd_open) {
        if (down)
            update_action(b);
        if (b == BTN_B)
            b_used = 1; /* so letting go of B doesn't roll a rhythm on PATTERN */
    }
    else if (b == BTN_B && page == PAGE_PATTERN) {
        if (down)
            b_used = 0;
        else if (!held[BTN_R2]) {
            pattern_b_released();
            changed = 1;
        }
    }
    else if (down)
        action(b, 0);
    update_fx();
    ui_unlock();
}

void ui_tick(int ms)
{
    now_ms += ms;
    if (toast_ms > 0)
        toast_ms -= ms;
    if (upd && upd->state == UP_AVAILABLE && !upd_asked)
        upd_asked = upd_open = 1;
    for (int b = 0; b < BTN_COUNT; b++) {
        if (!held[b] || !repeatable(b) || upd_open)
            continue;
        hold_ms[b] += ms;
        if (hold_ms[b] < 300)
            continue;
        rep_ms[b] += ms;
        int interval = hold_ms[b] > 1500 ? 35 : 75;
        while (rep_ms[b] >= interval) {
            rep_ms[b] -= interval;
            ui_lock();
            action(b, ++rep_n[b]);
            update_fx();
            ui_unlock();
        }
    }
    float k = expf(-ms / 120.f);
    for (int v = 0; v <= NV; v++) {
        float m = live.meter[v];
        meter_disp[v] = m > meter_disp[v] * k ? m : meter_disp[v] * k;
    }
    for (int v = 0; v < NV; v++) {
        uint32_t t = live.trig_count[v];
        if (t != seen_trig[v]) {
            seen_trig[v] = t;
            flash[v] = 1.f;
        }
        else
            flash[v] *= expf(-ms / 90.f);
    }
}

/* -------------------------------------------------------------- drawing */

static void text_c(int cx, int y, const char *s, uint32_t c, int font)
{
    gfx_text(cx - gfx_text_w(s, font) / 2, y, s, c, font);
}

static void text_r(int rx, int y, const char *s, uint32_t c, int font)
{
    gfx_text(rx - gfx_text_w(s, font), y, s, c, font);
}

static void play_icon(int x, int y, int h, uint32_t c)
{
    for (int j = 0; j < h; j++) {
        int w = (j < h / 2 ? j : h - 1 - j) + 1;
        gfx_fill(x, y + j, w, 1, c);
    }
}

static void badge(int *bx, const char *s, uint32_t bg, uint32_t fg)
{
    int w = gfx_text_w(s, FONT_SMALL) + 10;
    *bx -= w;
    gfx_fill(*bx, 11, w, 22, bg);
    gfx_text(*bx + 5, 11, s, fg, FONT_SMALL);
    *bx -= 6;
}

static void draw_header(void)
{
    gfx_fill(0, 0, SCREEN_W, HEADER_H, C_PANEL);
    /* the wordmark: a brick-orange block with COL ST */
    gfx_fill(8, 8, 6, 28, C_ACCENT);
    gfx_text(20, 2, "COL", C_TEXT, FONT_LARGE);
    gfx_text(20 + gfx_text_w("COL", FONT_LARGE) + 4, 2, "ST", C_ACCENT, FONT_LARGE);

    int x = 118;
    for (int p = 0; p < PAGE_COUNT; p++) {
        int w = gfx_text_w(page_names[p], FONT_SMALL) + 14;
        if (p == page)
            gfx_fill(x, 10, w, 24, C_ACCENT);
        else
            gfx_rect(x, 10, w, 24, 1, C_LINE);
        gfx_text(x + 7, 11, page_names[p], p == page ? 0x000000 : C_DIM, FONT_SMALL);
        x += w + 5;
    }

    char bpm[16];
    snprintf(bpm, sizeof bpm, "%.1f", song.bpm);
    int bw = gfx_text_w(bpm, FONT_LARGE);
    gfx_text(SCREEN_W - 8 - bw, 2, bpm, C_TEXT, FONT_LARGE);
    int ix = SCREEN_W - 8 - bw - 22, bx = ix - 8;
    /* the mute scenes: lit while on, dim while saved and off */
    for (int k = NSCENE - 1; k >= 0; k--)
        if (song.scene_used[k]) {
            char m[4];
            snprintf(m, sizeof m, "M%d", k + 1);
            badge(&bx, m, scene_on == k ? C_MUTE : C_PANEL2, scene_on == k ? 0x000000 : C_DIM);
        }
    if (half_on)
        badge(&bx, "1/2", 0x46b4ff, 0x000000);
    if (left_groove == 1) /* the Brick Pro's side switch up: the left stick plays the groove */
        badge(&bx, "GRV", 0x7ad87a, 0x000000);
    if (song.sync_mode != SYNC_OFF) {
        const SyncStatus *ss = sync_status();
        int linked = ss->leading || ss->have_leader;
        badge(&bx, ss->leading ? "LEAD" : ss->have_leader ? "FLW" : "SYNC", linked ? 0x46b4ff : C_PANEL2,
              linked ? 0x000000 : C_DIM);
    }
    if (live.playing)
        play_icon(ix, 12, 20, C_PLAY);
    else {
        gfx_fill(ix, 12, 5, 20, C_DIM);
        gfx_fill(ix + 9, 12, 5, 20, C_DIM);
    }

    int cur = live.step % 16;
    for (int i = 0; i < 16; i++) {
        uint32_t c = i % 4 == 0 ? C_LINE + 0x141414 : C_LINE;
        if (i == cur && live.playing)
            c = C_ACCENT;
        gfx_fill(i * 40 + 1, HEADER_H + 3, 38, 5, c);
    }
}

static void draw_footer(void)
{
    static const char *const hints[PAGE_COUNT] = {
        "A:ON  X/Y:HITS  B+^v:STEPS  B+<>:ROT  B:EUCLID",
        "<>:CHANNEL  X/Y:+/-  A:PLAY SLICE  B:MUTE",
        "DPAD:SELECT  X/Y:+/-  A:PLAY  B:MUTE",
        "X/Y:+/-  A:MUTE  B:SOLO  R2+L1/R1:SAVE M1/M2",
        "A:LOAD  X:SAVE  R2+Y:CLEAR",
    };
    const char *hint = hints[page];
    if (page == PAGE_SONG && song_cur >= NSLOT)
        hint = song_cur == SG_TEMPO ? "X/Y:TEMPO  R2+X:TAP" : song_cur == SG_SYNC ? "X/Y:SYNC MODE" : "<>:SETTING  X/Y:+/-";
    gfx_fill(0, FOOTER_Y, SCREEN_W, SCREEN_H - FOOTER_Y, C_PANEL);
    gfx_text(10, FOOTER_Y + 3, hint, C_DIM, FONT_SMALL);
    text_r(SCREEN_W - 10, FOOTER_Y + 3, "SEL:NEW BEAT", C_DIM, FONT_SMALL);
}

static void draw_toast(void)
{
    if (toast_ms <= 0)
        return;
    int w = gfx_text_w(toast, FONT_SMALL) + 28, h = 34;
    int x = (SCREEN_W - w) / 2, y = FOOTER_Y - h - 8;
    gfx_fill_a(x, y, w, h, 0x000000, 220);
    gfx_rect(x, y, w, h, 2, C_ACCENT);
    text_c(SCREEN_W / 2, y + 6, toast, C_TEXT, FONT_SMALL);
}

/* ------- PATTERN */

static void draw_cell(int x, int y, int w, int h, int v, int r, int sel)
{
    const Cell *c = &song.pat.cell[v][r];
    uint32_t col = vcol[v];
    int on = c->on;
    gfx_fill(x, y, w, h, sel ? C_PANEL2 : C_PANEL);
    gfx_rect(x, y, w, h, sel ? 2 : 1, sel ? C_TEXT : C_LINE);
    char s[24];
    snprintf(s, sizeof s, "%s %d/%d", row_names[r], c->hits, c->steps);
    gfx_text(x + 6, y + 3, s, on ? C_TEXT : C_DIM, FONT_SMALL);
    if (c->rot) {
        snprintf(s, sizeof s, ">%d", c->rot);
        text_r(x + w - 6, y + 3, s, C_DIM, FONT_SMALL);
    }
    int n = c->steps;
    int sx = x + 6, sw = w - 12, sy = y + 30, sh = h - 38;
    int cur = live.playing ? live.step % n : -1;
    for (int i = 0; i < n; i++) {
        int x0 = sx + sw * i / n, x1 = sx + sw * (i + 1) / n - 1;
        int hit = euclid_hit(c, i);
        uint32_t fc = hit ? (on ? col : C_LINE + 0x101010) : C_PANEL2;
        if (i == cur)
            fc = hit && on ? 0xffffff : C_DIM;
        gfx_fill(x0, sy + (hit ? 0 : sh / 3), x1 - x0, hit ? sh : sh / 3, fc);
    }
}

static void voice_summary(int v, char *s, int n)
{
    char a[16], b[16];
    if (v == V_BASS) {
        param_format(v, S_WAVE, a, sizeof a);
        param_format(v, S_ROOT, b, sizeof b);
        snprintf(s, n, "%s %s", a, b);
        return;
    }
    param_format(v, B_BANK, a, sizeof a);
    const int *P = song.pat.param[v];
    if (P[B_MODE] == MODE_GRID)
        snprintf(s, n, "%s GRID", a);
    else
        snprintf(s, n, "%s %d", a, P[B_SLICE] + 1);
}

static void draw_pattern(void)
{
    int rh = 78;
    for (int v = 0; v < NV; v++) {
        int y = BODY_Y + v * rh;
        uint32_t col = vcol[v];
        int muted = song.pat.mute[v];
        gfx_fill(4, y + 2, 112, rh - 6, C_PANEL);
        gfx_fill(4, y + 2, 5, rh - 6, gfx_mix(C_PANEL, col, 0.35f + 0.65f * flash[v]));
        gfx_text(16, y + 6, voice_names[v], muted ? C_DIM : col, FONT_SMALL);
        if (muted)
            gfx_text(62, y + 6, "MUTE", C_MUTE, FONT_SMALL);
        char s[40];
        voice_summary(v, s, sizeof s);
        gfx_text(16, y + 30, s, C_DIM, FONT_SMALL);
        float m = meter_disp[v];
        gfx_fill(16, y + rh - 14, 92, 4, C_PANEL2);
        gfx_fill(16, y + rh - 14, (int)(92 * fminf(1.f, m)), 4, col);
        for (int r = 0; r < NROW; r++)
            draw_cell(122 + r * 173, y + 2, 168, rh - 6, v, r, v == pat_v && r == pat_r);
    }
}

/* ------- CHOP */

static void draw_params(int v, int n, int sel, int x0, int y0, int colw, int rows, int rowh)
{
    for (int p = 0; p < n; p++) {
        int x = x0 + (p / rows) * colw, y = y0 + (p % rows) * rowh;
        int s = p == sel;
        if (s)
            gfx_fill(x, y, colw - 10, rowh - 4, C_PANEL2);
        gfx_rect(x, y, colw - 10, rowh - 4, s ? 2 : 1, s ? vcol[v] : C_LINE);
        gfx_text(x + 8, y + (rowh - 4 - 22) / 2, param_def(v, p)->name, s ? C_TEXT : C_DIM, FONT_SMALL);
        char val[24];
        param_format(v, p, val, sizeof val);
        text_r(x + colw - 18, y + (rowh - 4 - 22) / 2, val, C_TEXT, FONT_SMALL);
        /* the compressor's gain reduction under its value */
        if (v < NBRK && p == B_COMP && song.pat.param[v][B_COMP]) {
            float gr = fminf(1.f, live.gr[v] / 18.f);
            gfx_fill(x + 8, y + rowh - 9, (int)((colw - 26) * gr), 3, C_SOLO);
        }
    }
}

static void draw_chop(void)
{
    int v = chop_v;
    /* channel tabs */
    for (int i = 0; i < NBRK; i++) {
        int x = 8 + i * 157, w = 150;
        int s = i == v;
        gfx_fill(x, BODY_Y, w, 26, s ? vcol[i] : C_PANEL);
        char t[24], b[16];
        param_format(i, B_BANK, b, sizeof b);
        snprintf(t, sizeof t, "%s %s", voice_names[i], b);
        gfx_text(x + 8, BODY_Y + 2, t, s ? 0x000000 : (song.pat.mute[i] ? C_DIM : vcol[i]), FONT_SMALL);
        gfx_fill(x, BODY_Y + 24, (int)(w * flash[i]), 2, s ? 0xffffff : vcol[i]);
    }
    /* waveform with slice markers */
    int wx = 8, wy = BODY_Y + 32, ww = 624, wh = 120;
    gfx_fill(wx, wy, ww, wh, C_PANEL);
    if (!nbanks)
        return;
    const int *P = song.pat.param[v];
    const Bank *b = &banks[P[B_BANK] < nbanks ? P[B_BANK] : 0];
    int lo = P[B_SLICE], hi = P[B_SLICE];
    if (P[B_MODE] == MODE_WALK || P[B_MODE] == MODE_RAND)
        hi = lo + P[B_RANGE] - 1;
    else if (P[B_MODE] == MODE_GRID) {
        lo = 0;
        hi = b->nslices - 1;
    }
    for (int s = 0; s < b->nslices; s++) {
        int in = s >= lo && s <= hi;
        if (!in)
            continue;
        int x0 = wx + (int)((long long)ww * b->start[s] / b->len);
        int x1 = wx + (int)((long long)ww * b->start[s + 1] / b->len);
        gfx_fill(x0, wy, x1 - x0, wh, gfx_mix(C_PANEL, vcol[v], 0.18f));
    }
    int ls = live.slice[v];
    if (live.play_pos[v] >= 0.f && ls < b->nslices) {
        int x0 = wx + (int)((long long)ww * b->start[ls] / b->len);
        int x1 = wx + (int)((long long)ww * b->start[ls + 1] / b->len);
        gfx_fill(x0, wy, x1 - x0, wh, gfx_mix(C_PANEL, vcol[v], 0.45f));
    }
    int mid = wy + wh / 2;
    for (int i = 0; i < ww; i++) {
        float pk = b->peak[i * NPEAK / ww];
        int h = (int)(pk * (wh / 2 - 4));
        gfx_fill(wx + i, mid - h, 1, 2 * h + 1, gfx_mix(C_DIM, C_TEXT, pk));
    }
    for (int s = 0; s < b->nslices; s++) {
        int x = wx + (int)((long long)ww * b->start[s] / b->len);
        int in = s >= lo && s <= hi;
        gfx_fill(x, wy, 1, wh, in ? vcol[v] : C_LINE + 0x101010);
        uint32_t tc = b->tag[s] & TAG_KICK ? 0xff5a46 : b->tag[s] & TAG_SNARE ? 0xffd23c : 0x46b4ff;
        gfx_fill(x, wy + wh - 5, 5, 5, tc);
    }
    if (live.play_pos[v] >= 0.f)
        gfx_fill(wx + (int)(ww * live.play_pos[v]), wy, 2, wh, 0xffffff);
    char info[96];
    float fit = song.bpm / b->bpm;
    snprintf(info, sizeof info, "%.6s  %.0f BPM  %d STEPS  %d SLICES  FIT x%.2f", b->name, b->bpm, b->steps,
             b->nslices, fit);
    gfx_text(10, wy + wh + 4, info, C_DIM, FONT_SMALL);
    snprintf(info, sizeof info, "SLICE %d", ls + 1);
    text_r(SCREEN_W - 10, wy + wh + 4, info, vcol[v], FONT_SMALL);
    draw_params(v, NP, chop_p, 8, wy + wh + 30, 316, 7, 30);
}

/* ------- BASS */

static void draw_bass(void)
{
    const int *P = song.pat.param[V_BASS];
    uint32_t col = vcol[V_BASS];
    /* the riff as note boxes, the one sounding lit */
    int n = riff_len(P[S_RIFF]);
    int bw = 70, x0 = 8, y0 = BODY_Y + 4;
    gfx_text(x0, y0, "RIFF", C_DIM, FONT_SMALL);
    gfx_text(x0 + 50, y0, riff_name(P[S_RIFF]), col, FONT_SMALL);
    static const char *const nn[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int base = 12 * (P[S_OCT] + 1) + P[S_ROOT];
    for (int k = 0; k < n; k++) {
        int note = base + riff_note(P[S_RIFF], k);
        int x = x0 + k * (bw + 6), y = y0 + 28;
        int lit = live.note == note && flash[V_BASS] > 0.2f;
        gfx_fill(x, y, bw, 56, lit ? col : C_PANEL);
        gfx_rect(x, y, bw, 56, 1, C_LINE);
        char s[16];
        snprintf(s, sizeof s, "%s%d", nn[((note % 12) + 12) % 12], note / 12 - 1);
        text_c(x + bw / 2, y + 10, s, lit ? 0x000000 : C_TEXT, FONT_LARGE);
    }
    char s[32];
    int ln = live.note;
    snprintf(s, sizeof s, "%s%d", nn[((ln % 12) + 12) % 12], ln / 12 - 1);
    text_r(SCREEN_W - 12, y0 + 2, ln ? s : "--", gfx_mix(C_DIM, col, flash[V_BASS]), FONT_LARGE);
    float m = meter_disp[V_BASS];
    gfx_fill(8, y0 + 92, 624, 6, C_PANEL2);
    gfx_fill(8, y0 + 92, (int)(624 * fminf(1.f, m)), 6, col);
    draw_params(V_BASS, NBASS_P, bass_p, 8, y0 + 110, 316, (NBASS_P + 1) / 2, 46);
}

/* ------- MIX */

static void draw_mix(void)
{
    int cw = 104;
    for (int v = 0; v <= NV; v++) {
        int x = 6 + v * cw, sel = v == mix_v;
        uint32_t col = v < NV ? vcol[v] : C_TEXT;
        gfx_fill(x, BODY_Y, cw - 6, 388, sel ? C_PANEL2 : C_PANEL);
        if (sel)
            gfx_rect(x, BODY_Y, cw - 6, 388, 2, col);
        text_c(x + (cw - 6) / 2, BODY_Y + 4, v < NV ? voice_names[v] : "MASTER", col, FONT_SMALL);
        /* the fader and its meter */
        int lvl = v < NV ? song.vol[v] : song.master;
        int fx0 = x + 22, fy = BODY_Y + 34, fh = 160;
        gfx_fill(fx0, fy, 18, fh, C_BG);
        gfx_fill(fx0, fy + fh - fh * lvl / 100, 18, fh * lvl / 100, gfx_mix(C_LINE, col, 0.6f));
        float m = fminf(1.f, meter_disp[v]);
        gfx_fill(fx0 + 26, fy + fh - (int)(fh * m), 8, (int)(fh * m), m > 0.95f ? C_MUTE : C_PLAY);
        if (v == NV || (v < NBRK && song.pat.param[v][B_COMP])) {
            float gr = fminf(1.f, live.gr[v] / 18.f);
            gfx_fill(fx0 + 40, fy, 6, (int)(fh * gr), C_SOLO); /* gain reduction falls from the top */
        }
        int muted = v < NV && song.pat.mute[v], solo = v < NV && song.solo[v];
        if (muted)
            text_c(x + (cw - 6) / 2, fy + fh + 4, "MUTE", C_MUTE, FONT_SMALL);
        else if (solo)
            text_c(x + (cw - 6) / 2, fy + fh + 4, "SOLO", C_SOLO, FONT_SMALL);
        int nrows = v < NV ? NMXV : NMXM;
        for (int r = 0; r < nrows; r++) {
            int y = BODY_Y + 226 + r * 26;
            int s = sel && r == mix_r;
            if (s)
                gfx_fill(x + 4, y, cw - 14, 24, C_BG);
            const char *nm = v < NV ? mxv_names[r] : mxm_names[r];
            char val[16];
            if (v < NV) {
                int x2 = r == MX_LEVEL ? song.vol[v] : r == MX_PAN ? song.pan[v] : song.send[v];
                if (r == MX_PAN && song.out_mode == OUT_MONO_CLOCK)
                    x2 = 0; /* mono + clock: every channel from the centre (the pan is kept) */
                if (r == MX_PAN)
                    snprintf(val, sizeof val, x2 == 0 ? "C" : x2 < 0 ? "L%d" : "R%d", x2 < 0 ? -x2 : x2);
                else
                    snprintf(val, sizeof val, "%d", x2);
            }
            else {
                switch (r) {
                case MM_LEVEL: snprintf(val, sizeof val, "%d", song.master); break;
                case MM_GLUE: snprintf(val, sizeof val, "%d", song.glue); break;
                case MM_DRIVE: snprintf(val, sizeof val, "%d", song.master_drive); break;
                case MM_VERB: snprintf(val, sizeof val, "%d", song.verb_time); break;
                case MM_TONE: snprintf(val, sizeof val, "%d", song.verb_tone); break;
                default: snprintf(val, sizeof val, "%s", sweep_rate_name(song.sweep_rate)); break;
                }
            }
            gfx_text(x + 8, y + 1, nm, s ? C_TEXT : C_DIM, FONT_SMALL);
            text_r(x + cw - 14, y + 1, val, s ? col : C_TEXT, FONT_SMALL);
        }
    }
}

/* ------- SONG */

static void draw_song(void)
{
    for (int i = 0; i < NSLOT; i++) {
        int x = 8 + (i % 4) * 157, y = BODY_Y + 6 + (i / 4) * 92, w = 150, h = 84;
        int sel = song_cur == i, used = song.slot_used[i];
        int cur = live.cur_slot == i, queued = live.queued_slot == i;
        gfx_fill(x, y, w, h, cur ? gfx_mix(C_PANEL, C_ACCENT, 0.35f) : C_PANEL);
        gfx_rect(x, y, w, h, sel ? 3 : 1, sel ? C_TEXT : queued ? C_ACCENT : C_LINE);
        char s[4] = {(char)('A' + i), 0};
        gfx_text(x + 10, y + 6, s, used ? C_TEXT : C_DIM, FONT_LARGE);
        if (used) {
            /* a sketch of the stored hit cells */
            for (int v = 0; v < NV; v++) {
                const Cell *c = &song.slot[i].cell[v][ROW_HIT];
                for (int k = 0; k < 16; k++)
                    if (c->on && euclid_hit(c, k))
                        gfx_fill(x + 60 + k * 5, y + 14 + v * 12, 4, 9, vcol[v]);
            }
        }
        else
            gfx_text(x + 44, y + 30, "EMPTY", C_DIM, FONT_SMALL);
        if (queued)
            gfx_text(x + 10, y + h - 26, "NEXT", C_ACCENT, FONT_SMALL);
    }
    int y = BODY_Y + 200;
    const char *labels[2] = {"TEMPO", "WIFI SYNC"};
    for (int r = 0; r < 2; r++) {
        int yy = y + r * 44, sel = song_cur == SG_TEMPO + r;
        gfx_fill(8, yy, 624, 38, sel ? C_PANEL2 : C_PANEL);
        gfx_rect(8, yy, 624, 38, sel ? 2 : 1, sel ? C_ACCENT : C_LINE);
        gfx_text(20, yy + 8, labels[r], sel ? C_TEXT : C_DIM, FONT_SMALL);
        char val[48];
        if (r == 0)
            snprintf(val, sizeof val, "%.1f BPM", song.bpm);
        else {
            const SyncStatus *ss = sync_status();
            if (song.sync_mode == SYNC_OFF)
                snprintf(val, sizeof val, "OFF");
            else if (ss->have_leader)
                snprintf(val, sizeof val, "%s  FOLLOWING  %+.0f MS", sync_mode_names[song.sync_mode], ss->offset_ms);
            else if (ss->leading && ss->peers)
                snprintf(val, sizeof val, "%s  LEADING %d", sync_mode_names[song.sync_mode], ss->peers);
            else
                snprintf(val, sizeof val, "%s  ALONE", sync_mode_names[song.sync_mode]);
        }
        text_r(620, yy + 8, val, C_TEXT, FONT_SMALL);
    }
    /* the output: four settings in one row */
    {
        int yy = y + 88, row = song_cur == SG_OUT, clk = song.out_mode == OUT_MONO_CLOCK;
        static const char *const nm[4] = {"OUTPUT", "CLOCK ON", "PPQN", "PULSE"};
        for (int k = 0; k < 4; k++) {
            int x = 8 + k * 157, w = 150, sel = row && out_f == k, on = k == 0 || clk;
            gfx_fill(x, yy, w, 38, sel ? C_PANEL2 : C_PANEL);
            gfx_rect(x, yy, w, 38, sel ? 2 : 1, sel ? C_ACCENT : C_LINE);
            gfx_text(x + 10, yy + 8, nm[k], sel ? C_TEXT : C_DIM, FONT_SMALL);
            char val[24];
            if (k == 0)
                snprintf(val, sizeof val, "%s", clk ? "MONO+CLK" : "STEREO");
            else if (k == 1)
                snprintf(val, sizeof val, "%s", song.clk_side ? "LEFT" : "RIGHT");
            else if (k == 2)
                snprintf(val, sizeof val, "%d", clk_ppqn_value(song.clk_ppqn));
            else
                snprintf(val, sizeof val, "%dMS", song.clk_width);
            text_r(x + w - 10, yy + 8, val, on ? (k == 0 && clk ? C_ACCENT : C_TEXT) : C_DIM, FONT_SMALL);
        }
    }
    char s[96];
    snprintf(s, sizeof s, "%d BREAKS, %d BASS SAMPLES LOADED", nbanks, nbass);
    gfx_text(12, y + 140, s, C_DIM, FONT_SMALL);
    gfx_text(12, y + 164, "SLOTS LOAD AT THE NEXT BAR WHILE PLAYING", C_DIM, FONT_SMALL);
}

static void draw_fx_overlay(void)
{
    static const char *const names[] = {"REPEAT 1/8", "REPEAT 1/16", "REVERSE", "TAPE STOP"};
    if (live.fx >= 0) {
        int w = gfx_text_w(names[live.fx], FONT_LARGE) + 30;
        int x = (SCREEN_W - w) / 2, y = 190;
        gfx_fill_a(x, y, w, 52, 0x000000, 200);
        gfx_rect(x, y, w, 52, 2, C_ACCENT);
        text_c(SCREEN_W / 2, y + 8, names[live.fx], C_ACCENT, FONT_LARGE);
    }
    if (live.cutoff_norm < 0.99f) {
        int w = (int)(SCREEN_W * live.cutoff_norm);
        gfx_fill(0, HEADER_H + 9, w, 3, C_SOLO);
    }
}

static void draw_update(void)
{
    if (!upd_open)
        return;
    int st = upd->state, w = 500, h = 230, x = (SCREEN_W - w) / 2, y = 120;
    char s[200];
    gfx_fill_a(0, 0, SCREEN_W, SCREEN_H, 0x000000, 150);
    gfx_fill(x, y, w, h, C_PANEL);
    gfx_rect(x, y, w, h, 2, C_ACCENT);
    snprintf(s, sizeof s, "V%s IS OUT", upd->latest);
    text_c(SCREEN_W / 2, y + 16, s, C_ACCENT, FONT_LARGE);
    text_c(SCREEN_W / 2, y + 58, "THIS IS V" CS_VERSION, C_DIM, FONT_SMALL);
    if (st == UP_INSTALLING || st == UP_INSTALLED) {
        int bw = 360, bx = (SCREEN_W - bw) / 2, p = st == UP_INSTALLED ? 100 : clampi(upd->progress, 0, 100);
        gfx_rect(bx, y + 104, bw, 16, 1, C_LINE);
        gfx_fill(bx + 2, y + 106, (bw - 4) * p / 100, 12, C_ACCENT);
        if (st == UP_INSTALLED)
            snprintf(s, sizeof s, "UPDATED - RESTARTING...");
        else
            snprintf(s, sizeof s, "DOWNLOADING... %d%%", p);
        text_c(SCREEN_W / 2, y + 136, s, C_TEXT, FONT_SMALL);
        return;
    }
    if (st == UP_FAILED) {
        int n = snprintf(s, sizeof s, "%s", upd->err);
        for (int i = 0; s[i]; i++)
            s[i] = (char)toupper((unsigned char)s[i]);
        while (n > 3 && gfx_text_w(s, FONT_SMALL) > w - 24) /* a long path: cut it short */
            memcpy(s + (n -= 4), "...", 4);
        text_c(SCREEN_W / 2, y + 100, s, C_MUTE, FONT_SMALL);
        text_c(SCREEN_W / 2, y + 126, "NOTHING WAS CHANGED", C_DIM, FONT_SMALL);
        text_c(SCREEN_W / 2, y + h - 40, "A:TRY AGAIN  B:CLOSE", C_TEXT, FONT_SMALL);
        return;
    }
    text_c(SCREEN_W / 2, y + 100, "DOWNLOAD IT AND RESTART?", C_TEXT, FONT_SMALL);
    text_c(SCREEN_W / 2, y + 126, "YOUR SONG AND BREAKS STAY AS THEY ARE", C_DIM, FONT_SMALL);
    text_c(SCREEN_W / 2, y + h - 40, "A:UPDATE AND RESTART  B:NOT NOW", C_TEXT, FONT_SMALL);
}

void ui_draw(uint32_t *fb)
{
    gfx_target(fb);
    gfx_clear(C_BG);
    draw_header();
    switch (page) {
    case PAGE_PATTERN: draw_pattern(); break;
    case PAGE_CHOP: draw_chop(); break;
    case PAGE_BASS: draw_bass(); break;
    case PAGE_MIX: draw_mix(); break;
    case PAGE_SONG: draw_song(); break;
    }
    draw_footer();
    draw_fx_overlay();
    draw_update();
    draw_toast();
}

void ui_draw_loading(uint32_t *fb, const char *msg)
{
    gfx_target(fb);
    gfx_clear(C_BG);
    gfx_fill(250, 200, 10, 60, C_ACCENT);
    gfx_text(270, 196, "COLUMBIA", C_TEXT, FONT_LARGE);
    gfx_text(270, 230, "STREET", C_ACCENT, FONT_LARGE);
    text_c(SCREEN_W / 2, 300, msg, C_DIM, FONT_SMALL);
}

/* ---------------------------------------------------------------- MIDI CC */

/* The important channel, mixer, pattern and play settings have a MIDI CC,
 * all on one channel (any channel is heard). The channel's settings act on
 * the one CC 2 chooses (1-4 the breaks, 5 the bass). The numbers are ones
 * MIDI leaves free, in blocks: 14-26 the channel's params (in the order of
 * its page), 27-31 its mixer strip, 85-90 the tempo and the bus, 102-113
 * its HIT, ROLL and ALT cells, 114-116 the scenes and the pattern slot.
 * A CC's 0-127 spans the setting's range; an on / off is on from 64. */
enum { CCK_CHANNEL, CCK_PARAM, CCK_LEVEL, CCK_PAN, CCK_SEND, CCK_MUTE, CCK_SOLO, CCK_BPM, CCK_MASTER, CCK_GLUE,
       CCK_DRIVE, CCK_VERB, CCK_VTONE, CCK_CELL, CCK_SCENE, CCK_SLOT };
typedef struct {
    unsigned char cc, kind, a, b; /* a, b: the param, or the row and field (0 on, 1 steps, 2 hits, 3 rotate) */
} CcMap;
static const CcMap cc_map[] = {
    {2, CCK_CHANNEL, 0, 0},
    {14, CCK_PARAM, 0, 0},  {15, CCK_PARAM, 1, 0},  {16, CCK_PARAM, 2, 0},  {17, CCK_PARAM, 3, 0},
    {18, CCK_PARAM, 4, 0},  {19, CCK_PARAM, 5, 0},  {20, CCK_PARAM, 6, 0},  {21, CCK_PARAM, 7, 0},
    {22, CCK_PARAM, 8, 0},  {23, CCK_PARAM, 9, 0},  {24, CCK_PARAM, 10, 0}, {25, CCK_PARAM, 11, 0},
    {26, CCK_PARAM, 12, 0},
    {27, CCK_LEVEL, 0, 0},  {28, CCK_PAN, 0, 0},    {29, CCK_SEND, 0, 0},   {30, CCK_MUTE, 0, 0},
    {31, CCK_SOLO, 0, 0},
    {85, CCK_BPM, 0, 0},    {86, CCK_MASTER, 0, 0}, {87, CCK_GLUE, 0, 0},   {88, CCK_DRIVE, 0, 0},
    {89, CCK_VERB, 0, 0},   {90, CCK_VTONE, 0, 0},
    {102, CCK_CELL, ROW_HIT, 0},  {103, CCK_CELL, ROW_HIT, 1},  {104, CCK_CELL, ROW_HIT, 2},
    {105, CCK_CELL, ROW_HIT, 3},  {106, CCK_CELL, ROW_ROLL, 0}, {107, CCK_CELL, ROW_ROLL, 1},
    {108, CCK_CELL, ROW_ROLL, 2}, {109, CCK_CELL, ROW_ROLL, 3}, {110, CCK_CELL, ROW_ALT, 0},
    {111, CCK_CELL, ROW_ALT, 1},  {112, CCK_CELL, ROW_ALT, 2},  {113, CCK_CELL, ROW_ALT, 3},
    {114, CCK_SCENE, 0, 0}, {115, CCK_SCENE, 1, 0}, {116, CCK_SLOT, 0, 0},
};
#define NCC_MAP ((int)(sizeof cc_map / sizeof cc_map[0]))
static int cc_v; /* the channel the channel CCs act on */

static int cc_scale(int v, int lo, int hi) { return lo + ((hi - lo) * v + 63) / 127; }

int ui_midi_cc(int ch, int cc, int v)
{
    (void)ch;
    const CcMap *m = NULL;
    for (int i = 0; i < NCC_MAP; i++)
        if (cc_map[i].cc == cc)
            m = &cc_map[i];
    if (!m)
        return 0;
    v = clampi(v, 0, 127);
    int on = v >= 64;
    ui_lock();
    switch (m->kind) {
    case CCK_CHANNEL: {
        int nv = v * NV / 128;
        if (nv != cc_v) {
            cc_v = nv;
            say("MIDI: %s", voice_names[cc_v]);
        }
        break;
    }
    case CCK_PARAM:
        if (m->a < nparam(cc_v) && param_max(cc_v, m->a) > param_min(cc_v, m->a)) {
            int x = cc_scale(v, param_min(cc_v, m->a), param_max(cc_v, m->a));
            if (x != song.pat.param[cc_v][m->a])
                param_set(cc_v, m->a, x);
        }
        break;
    case CCK_LEVEL: song.vol[cc_v] = cc_scale(v, 0, 100); break;
    case CCK_PAN: song.pan[cc_v] = cc_scale(v, -100, 100); break;
    case CCK_SEND: song.send[cc_v] = cc_scale(v, 0, 100); break;
    case CCK_MUTE: song.pat.mute[cc_v] = (uint8_t)on; break;
    case CCK_SOLO: song.solo[cc_v] = (uint8_t)on; break;
    case CCK_BPM: song.bpm = (float)cc_scale(v, 40, 250); break;
    case CCK_MASTER: song.master = cc_scale(v, 0, 100); break;
    case CCK_GLUE: song.glue = cc_scale(v, 0, 100); break;
    case CCK_DRIVE: song.master_drive = cc_scale(v, 0, 100); break;
    case CCK_VERB: song.verb_time = cc_scale(v, 0, 100); break;
    case CCK_VTONE: song.verb_tone = cc_scale(v, 0, 100); break;
    case CCK_CELL: {
        Cell *c = &song.pat.cell[cc_v][m->a];
        switch (m->b) {
        case 0: c->on = (uint8_t)on; break;
        case 1: c->steps = (uint8_t)cc_scale(v, 1, MAX_STEPS); break;
        case 2: c->hits = (uint8_t)cc_scale(v, 0, c->steps); break;
        case 3: c->rot = (uint8_t)cc_scale(v, 0, c->steps - 1); break;
        }
        if (c->hits > c->steps)
            c->hits = c->steps;
        c->rot %= c->steps;
        break;
    }
    case CCK_SCENE:
        if (on != (scene_on == m->a))
            scene_toggle(m->a);
        break;
    case CCK_SLOT: {
        int i = v * NSLOT / 128;
        if (song.slot_used[i] && i != live.cur_slot && i != live.queued_slot) {
            slot_load(i);
            say(live.playing ? "%c AT THE NEXT BAR" : "LOADED %c", 'A' + i);
        }
        break;
    }
    }
    changed = 1;
    ui_unlock();
    return 1;
}

/* the implementation chart's CC table, as Markdown */
void ui_midi_chart(FILE *f)
{
    static const char *const names[] = {"CHANNEL: the one the channel CCs act on", "", "Channel LEVEL",
                                        "Channel PAN", "Channel SEND (reverb)", "Channel MUTE", "Channel SOLO",
                                        "BPM", "MASTER", "GLUE", "DRIVE", "VERB (reverb time)", "VERB TONE", "",
                                        "Scene", "Pattern SLOT"};
    static const char *const rows[NROW] = {"HIT", "ROLL", "ALT"}, *const fields[4] = {"ON", "STEPS", "HITS", "ROTATE"};
    fprintf(f, "| CC | Setting | Range |\n|---|---|---|\n");
    for (int i = 0; i < NCC_MAP; i++) {
        const CcMap *m = &cc_map[i];
        switch (m->kind) {
        case CCK_CHANNEL:
            fprintf(f, "| %d | %s | 0-25 BREAK 1, 26-50 BREAK 2, 51-76 BREAK 3, 77-101 BREAK 4, 102-127 BASS |\n",
                    m->cc, names[m->kind]);
            break;
        case CCK_PARAM: {
            /* a range of 0 to 0 is one that depends on what is loaded */
            char rb[48], rs[48];
            const ParamDef *b = param_def(0, m->a), *s = param_def(V_BASS, m->a);
            if (b->max > b->min)
                snprintf(rb, sizeof rb, "%s %d to %d", b->name, b->min, b->max);
            else
                snprintf(rb, sizeof rb, "%s (what is loaded)", b->name);
            if (m->a >= nparam(V_BASS))
                snprintf(rs, sizeof rs, "nothing");
            else if (s->max > s->min)
                snprintf(rs, sizeof rs, "%s %d to %d", s->name, s->min, s->max);
            else
                snprintf(rs, sizeof rs, "%s (what is loaded)", s->name);
            fprintf(f, "| %d | Channel param %d | breaks: %s; bass: %s |\n", m->cc, m->a + 1, rb, rs);
            break;
        }
        case CCK_MUTE:
        case CCK_SOLO: fprintf(f, "| %d | %s | 0-63 off, 64-127 on |\n", m->cc, names[m->kind]); break;
        case CCK_PAN: fprintf(f, "| %d | %s | -100 to 100 |\n", m->cc, names[m->kind]); break;
        case CCK_BPM: fprintf(f, "| %d | %s | 40 to 250 |\n", m->cc, names[m->kind]); break;
        case CCK_CELL:
            fprintf(f, "| %d | Channel %s cell %s | %s |\n", m->cc, rows[m->a], fields[m->b],
                    m->b == 0 ? "0-63 off, 64-127 on" : m->b == 1 ? "1 to 32" : m->b == 2 ? "0 to its steps"
                                                                                       : "0 to its steps - 1");
            break;
        case CCK_SCENE: fprintf(f, "| %d | %s M%d | 0-63 off, 64-127 on (if saved) |\n", m->cc, names[m->kind], m->a + 1); break;
        case CCK_SLOT: fprintf(f, "| %d | %s | 0-15 A, 16-31 B ... 112-127 H (saved slots; at the next bar while playing) |\n", m->cc, names[m->kind]); break;
        default: fprintf(f, "| %d | %s | 0 to 100 |\n", m->cc, names[m->kind]); break;
        }
    }
}
