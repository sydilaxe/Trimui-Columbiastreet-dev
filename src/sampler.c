/* The sampler: see sampler.h. */
#define _GNU_SOURCE
#include "sampler.h"
#include "breaks.h"
#include "engine.h"

#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int sr = 48000;
static char dir[512] = "rec";
static float *buf; /* mono, at the engine rate */
static int cap;
static atomic_int lat_frames;
static int sync_mode;

/* what the main thread asked for, and how far the audio thread got: the
 * state is the hand-over (the audio thread moves ARMED -> RECORDING ->
 * CHOPPING, the main thread the rest) */
static atomic_int state = REC_IDLE, cancel_req, stop_req;
static int ch, bars, src;
static atomic_int mon_src; /* the meter's source while idle */
static double target;   /* the bar line it starts on (audio thread's) */
static int link_wait;   /* LINK armed: frames waited for the sender's position */
static int want, got;   /* frames: the length (0: FREE), and recorded so far */
static float rec_bpm;   /* the tempo it was recorded at (0: free, not playing) */
static _Atomic float level;
static atomic_int prog; /* got, for the screen */

void sampler_init(int rate, const char *d)
{
    sr = rate;
    if (d)
        snprintf(dir, sizeof dir, "%s", d);
    cap = (int)(REC_MAX_SECS * rate);
    free(buf);
    buf = calloc((size_t)cap, sizeof(float));
    if (!buf)
        cap = 0;
    atomic_store(&state, REC_IDLE);
}

void sampler_set_latency(int frames) { atomic_store(&lat_frames, frames < 0 ? 0 : frames); }
void sampler_sync_mode(int on) { sync_mode = on; }
void sampler_monitor(int s) { atomic_store(&mon_src, s); }

/* ------------------------------------------------------------- audio side */

void sampler_audio(const float *in, const float *link, double link_pos, const int16_t *out, int frames, double pos0,
                   double spf)
{
    int st = atomic_load_explicit(&state, memory_order_acquire);
    if (st != REC_RECORDING) { /* the meter, before anything is recorded: is there sound to sample? */
        int ms = st == REC_ARMED ? src : atomic_load_explicit(&mon_src, memory_order_relaxed);
        float pk = 0.f;
        for (int k = 0; k < frames; k++) {
            /* either side: sound on the left alone shows (and records, half as loud) */
            const float *f = ms == SRC_LINK ? link : ms == SRC_IN ? in : NULL;
            float l = f ? f[2 * k] : ms == SRC_OUT ? out[2 * k] / 32768.f : 0.f;
            float r = f ? f[2 * k + 1] : ms == SRC_OUT ? out[2 * k + 1] / 32768.f : 0.f;
            pk = fmaxf(pk, fmaxf(fabsf(l), fabsf(r)));
        }
        if (pk > atomic_load(&level))
            atomic_store(&level, pk);
    }
    if (st != REC_ARMED && st != REC_RECORDING)
        return;
    if (atomic_load(&cancel_req)) {
        atomic_store(&cancel_req, 0);
        atomic_store_explicit(&state, REC_IDLE, memory_order_release);
        return;
    }
    int i = 0;
    if (st == REC_ARMED) {
        /* the position each frame stands for: the output's own; the input's
         * as long before as it is late (what was heard then is what came in);
         * the link's, the sender's own (its tempo is this one's: it leads) */
        double lag = src == SRC_IN ? atomic_load(&lat_frames) : 0.0, p0 = pos0;
        if (src == SRC_LINK && link && link_pos >= 0.0)
            p0 = link_pos, lag = 0.0, spf = song.bpm / 15.0 / sr;
        else if (src == SRC_LINK && isnan(target) && (link_wait += frames) < sr / 4)
            return; /* a moment for the sender's position (none: it isn't playing), then this Brick's own bar */
        if (isnan(target))
            target = ceil((p0 - lag * spf) / 16.0 - 1e-9) * 16.0;
        for (; i < frames; i++)
            if (p0 + (i - lag) * spf >= target)
                break;
        if (i >= frames)
            return;
        want = (int)llround(bars * 16.0 * 15.0 * sr / song.bpm);
        if (want > cap)
            want = cap;
        rec_bpm = song.bpm;
        got = 0;
        atomic_store_explicit(&state, REC_RECORDING, memory_order_release);
    }
    float pk = 0.f;
    for (; i < frames && got < cap && (!want || got < want); i++) {
        float x;
        if (src == SRC_OUT)
            x = (out[2 * i] + out[2 * i + 1]) * (0.5f / 32768.f);
        else if (src == SRC_LINK)
            x = link ? 0.5f * (link[2 * i] + link[2 * i + 1]) : 0.f;
        else
            x = in ? 0.5f * (in[2 * i] + in[2 * i + 1]) : 0.f;
        buf[got++] = x;
        pk = fmaxf(pk, fabsf(x));
    }
    if (pk > atomic_load(&level))
        atomic_store(&level, pk);
    atomic_store(&prog, got);
    if ((want && got >= want) || got >= cap || atomic_load(&stop_req)) {
        atomic_store(&stop_req, 0);
        atomic_store_explicit(&state, REC_CHOPPING, memory_order_release);
    }
}

/* -------------------------------------------------------------- main side */

int sampler_state(void) { return atomic_load(&state); }
int sampler_channel(void) { return ch; }

float sampler_progress(void)
{
    int n = want ? want : cap;
    return n ? fminf(1.f, (float)atomic_load(&prog) / n) : 0.f;
}

float sampler_level(void)
{
    float l = atomic_load(&level);
    atomic_store(&level, l * 0.9f); /* falls between frames */
    return fminf(1.f, l);
}

int sampler_arm(int v, int nbars, int source)
{
    if (!cap || atomic_load(&state) != REC_IDLE)
        return 0;
    ch = v, bars = nbars, src = source;
    target = NAN, link_wait = 0;
    got = 0, want = 0;
    atomic_store(&prog, 0);
    atomic_store(&cancel_req, 0), atomic_store(&stop_req, 0);
    if (!live.playing || !nbars) {
        /* stopped, or FREE: from now. Stopped and in time, that many bars at the
         * tempo; FREE until the next press */
        want = nbars ? (int)llround(nbars * 16.0 * 15.0 * sr / song.bpm) : 0;
        if (want > cap)
            want = cap;
        rec_bpm = nbars || live.playing ? song.bpm : 0.f;
        atomic_store_explicit(&state, REC_RECORDING, memory_order_release);
        return 1;
    }
    atomic_store_explicit(&state, REC_ARMED, memory_order_release);
    return 1;
}

void sampler_press(void)
{
    int st = atomic_load(&state);
    if (st == REC_ARMED || (st == REC_RECORDING && want))
        atomic_store(&cancel_req, 1);
    else if (st == REC_RECORDING)
        atomic_store(&stop_req, 1);
}

/* ---------------------------------------------------- cutting and keeping */

static int write_wav(const char *path, const float *d, int n)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;
    uint32_t bytes = (uint32_t)n * 2u, r = (uint32_t)sr, br = r * 2u;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 1, 0};
    uint32_t riff = 36 + bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 24, &r, 4);
    memcpy(h + 28, &br, 4);
    h[32] = 2, h[34] = 16;
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &bytes, 4);
    int ok = fwrite(h, 1, 44, f) == 44;
    int16_t blk[1024];
    for (int i = 0; ok && i < n; i += 1024) {
        int m = n - i < 1024 ? n - i : 1024;
        for (int k = 0; k < m; k++) {
            float x = d[i + k] * 32767.f;
            blk[k] = (int16_t)(x > 32767.f ? 32767 : x < -32768.f ? -32768 : lrintf(x));
        }
        ok = fwrite(blk, 2, (size_t)m, f) == (size_t)m;
    }
    ok = fclose(f) == 0 && ok;
    if (ok)
        ok = rename(tmp, path) == 0;
    else
        unlink(tmp);
    return ok;
}

/* the files kept for RECk, whatever their length or cut */
static void rec_files(int k, void (*fn)(const char *path, void *ud), void *ud)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    char pre[16], pre2[16];
    snprintf(pre, sizeof pre, "REC%d_", k);
    snprintf(pre2, sizeof pre2, "REC%d.", k);
    struct dirent *e;
    while ((e = readdir(d)))
        if (!strncmp(e->d_name, pre, strlen(pre)) || !strncmp(e->d_name, pre2, strlen(pre2))) {
            char p[1100];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            fn(p, ud);
        }
    closedir(d);
}
static void remove_one(const char *path, void *ud) { (void)ud; unlink(path); }
static void rename_one(const char *path, void *ud) { rename(path, (const char *)ud); }

/* the cut, on a thread of its own (it reads every sample a few times over) */
typedef struct {
    Bank b;
    float *keep; /* a copy to write to the card, after the bank is in */
    int len, k, bars;
    int to_set;      /* one more break in the set (label, path), not the channel's RECk */
    char label[8], path[1100], set[SET_NAME];
    atomic_int ready, ok;
} Job;
static Job job;
static atomic_int job_busy;

static void *cut_main(void *arg)
{
    (void)arg;
    bank_from_loop(&job.b, job.label, job.b.data, job.len, rec_bpm);
    /* recorded in time: exactly its bars of 16ths long */
    if (job.bars > 0)
        job.b.steps = job.bars * 16;
    job.keep = malloc((size_t)job.len * sizeof(float));
    if (job.keep)
        memcpy(job.keep, job.b.data, (size_t)job.len * sizeof(float));
    atomic_store(&job.ok, 1);
    atomic_store_explicit(&job.ready, 1, memory_order_release);
    if (job.keep && job.to_set) { /* kept in the set's folder, a break of the set's own from now on */
        if (!write_wav(job.path, job.keep, job.len))
            fprintf(stderr, "sampler: couldn't keep %s\n", job.path);
        free(job.keep);
        job.keep = NULL;
    }
    if (job.keep) { /* kept in rec/, replacing the last for this channel */
        mkdir(dir, 0755);
        rec_files(job.k, remove_one, NULL);
        char path[1100];
        breaks_rec_file(path, sizeof path, dir, job.k, job.bars, CHOP_HITS);
        if (!write_wav(path, job.keep, job.len))
            fprintf(stderr, "sampler: couldn't keep %s\n", path);
        free(job.keep);
        job.keep = NULL;
    }
    atomic_store(&job_busy, 0);
    return NULL;
}

static int find_bank(const char *name)
{
    for (int i = 0; i < nbanks; i++)
        if (!strcmp(banks[i].name, name))
            return i;
    return -1;
}

int sampler_poll(void (*lock)(void), void (*unlock)(void), char *msg, int n)
{
    int st = atomic_load_explicit(&state, memory_order_acquire);
    if (st != REC_CHOPPING)
        return 0;
    if (!atomic_load(&job.ready)) {
        if (atomic_load(&job_busy))
            return 0;
        if (got < sr / 20) { /* under 50 ms: nothing worth keeping */
            atomic_store(&state, REC_IDLE);
            snprintf(msg, (size_t)n, "REC%d: TOO SHORT, NOTHING KEPT", ch + 1);
            return 1;
        }
        memset(&job.b, 0, sizeof job.b);
        job.b.data = malloc((size_t)got * sizeof(float));
        if (!job.b.data) {
            atomic_store(&state, REC_IDLE);
            snprintf(msg, (size_t)n, "REC%d: NO MEMORY", ch + 1);
            return 1;
        }
        memcpy(job.b.data, buf, (size_t)got * sizeof(float));
        job.len = got, job.k = ch + 1, job.bars = want ? bars : 0;
        /* one more break in the set when it has room (not DIRT), else the channel's RECk */
        job.to_set = breaks_set_add_name(job.path, sizeof job.path, job.label, job.bars);
        if (!job.to_set)
            snprintf(job.label, sizeof job.label, "REC%d", job.k);
        snprintf(job.set, sizeof job.set, "%s", breaks_set_name(breaks_set_current()));
        atomic_store(&job.ready, 0), atomic_store(&job.ok, 0);
        atomic_store(&job_busy, 1);
        pthread_t t;
        if (sync_mode || pthread_create(&t, NULL, cut_main, NULL) != 0)
            cut_main(NULL);
        else
            pthread_detach(t);
        if (!atomic_load(&job.ready))
            return 0;
    }
    /* cut: in under the lock: in the set, in place of the empty set's stand-in or after the rest;
     * RECk in place of the last or after the rest. The set switched meanwhile: kept on the card only */
    float *old = NULL;
    int moved = job.to_set && strcmp(job.set, breaks_set_name(breaks_set_current()));
    lock();
    int i = moved ? -1 : find_bank(job.to_set ? EMPTY_BANK : job.label);
    if (i >= 0) {
        old = banks[i].data;
        banks[i] = job.b;
        engine_bank_replaced(i);
    }
    else if (!moved && nbanks < MAX_BANKS) {
        i = nbanks;
        banks[nbanks++] = job.b;
    }
    if (i >= 0) {
        param_set(ch, B_BANK, i);
        if (song.pat.param[ch][B_MODE] == MODE_FIX && job.bars > 0)
            song.pat.param[ch][B_MODE] = MODE_GRID; /* in time: plays as recorded, cut on the grid */
        engine_bank_replaced(i);
    }
    unlock();
    if (i < 0)
        free(job.b.data);
    free(old);
    atomic_store(&job.ready, 0);
    atomic_store(&state, REC_IDLE);
    char who[32];
    if (job.to_set)
        snprintf(who, sizeof who, "%s IN %.12s", job.label, job.set);
    else
        snprintf(who, sizeof who, "%s", job.label);
    if (moved)
        snprintf(msg, (size_t)n, "%s: KEPT (THE SET CHANGED)", who);
    else if (i < 0)
        snprintf(msg, (size_t)n, "%s: NO ROOM FOR ANOTHER BREAK", who);
    else if (job.bars)
        snprintf(msg, (size_t)n, "%s: %d BAR%s, %d SLICES", who, job.bars, job.bars > 1 ? "S" : "", banks[i].nslices);
    else
        snprintf(msg, (size_t)n, "%s: %.1f S, %d SLICES", who, (double)job.len / sr, banks[i].nslices);
    return 1;
}

int sampler_chop(int v, int style, void (*lock)(void), void (*unlock)(void))
{
    if (!nbanks)
        return 0;
    int i = song.pat.param[v][B_BANK];
    if (i < 0 || i >= nbanks)
        return 0;
    static Bank t; /* the cut worked out aside: the samples aren't touched */
    t = banks[i];
    if (!bank_chop(&t, style))
        return 0;
    lock();
    Bank *b = &banks[i];
    b->nslices = t.nslices;
    memcpy(b->start, t.start, sizeof b->start);
    memcpy(b->at, t.at, sizeof b->at);
    memcpy(b->tag, t.tag, sizeof b->tag);
    b->chop = t.chop;
    song_clamp();
    unlock();
    /* a recording keeps its cut in its file's name */
    int k = 0;
    if (sscanf(b->name, "REC%d", &k) == 1 && k >= 1 && k <= NREC) {
        char path[1100];
        breaks_rec_file(path, sizeof path, dir, k, t.timed ? t.steps / 16 : 0, style);
        rec_files(k, rename_one, path);
    }
    return 1;
}
