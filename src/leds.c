/* The TrimUI's LEDs: see leds.h. */
#include "leds.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"

enum { Z_F1, Z_F2, Z_M, Z_LR, NZONE };
static const char *const zone_name[NZONE] = {"f1", "f2", "m", "lr"};
/* the brightness each zone's colour is scaled by, in the driver: shared by F1 and F2 */
static const char *const scale_name[] = {"max_scale_f1f2", "max_scale", "max_scale_lr"};
#define NSCALE 3
#define LED_SCALE 60 /* bright enough to read in daylight, not glaring */

/* the channels' colours, as on screen */
static const uint32_t chan_col[NV] = {0xff5a46, 0xffb428, 0x5adc8c, 0x46b4ff, 0xb478ff};
#define COL_SCENE 0xff2030 /* a mute scene on: the screen's MUTE red */
#define COL_BAR 0xff6414   /* the bar's first beat: the wordmark's orange */
#define COL_BEAT 0xb4b4b4  /* the other beats */
#define COL_CLIP 0xff0000

static char dir[256] = "/sys/class/led_anim";
static int on;
static pthread_t thread;
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static volatile int running;
static uint32_t want[NZONE], written[NZONE];
static int have_written[NZONE];
/* what was there at start, to put back; kept in SAVED_FILE until it is put
 * back, so a crash or a kill doesn't lose the user's own LED settings (the
 * next start takes them from there) */
#define SAVED_FILE "leds_saved.txt"
static char saved_col[NZONE][16], saved_eff[NZONE][8], saved_scale[NSCALE][8], saved_enable[8]; /* as read: short */

static int put(const char *name, const char *val)
{
    char p[320];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f)
        return 0;
    int ok = fputs(val, f) >= 0 && (val[0] && val[strlen(val) - 1] == '\n' ? 1 : fputc('\n', f) != EOF);
    return fclose(f) == 0 && ok;
}

static void get(const char *name, char *out, int n)
{
    char p[320];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    out[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    if (!fgets(out, n, f))
        out[0] = 0;
    out[strcspn(out, " \n")] = 0;
    fclose(f);
}

/* a zone's colour: the driver takes RRGGBB, and a new colour shows once the
 * effect is written again (4: static) */
static void write_zone(int z, uint32_t c)
{
    char name[32], v[16];
    snprintf(name, sizeof name, "effect_rgb_hex_%s", zone_name[z]);
    snprintf(v, sizeof v, "%06X\n", (unsigned)(c & 0xffffff));
    put(name, v);
    snprintf(name, sizeof name, "effect_%s", zone_name[z]);
    put(name, "4\n");
}

void leds_flush(void)
{
    uint32_t w[NZONE];
    pthread_mutex_lock(&mx);
    memcpy(w, want, sizeof w);
    pthread_mutex_unlock(&mx);
    for (int z = 0; z < NZONE; z++)
        if (!have_written[z] || w[z] != written[z]) {
            write_zone(z, w[z]);
            written[z] = w[z];
            have_written[z] = 1;
        }
}

static void *led_thread(void *arg)
{
    (void)arg;
    while (running) {
        leds_flush();
        usleep(25000);
    }
    return NULL;
}

int leds_start(void)
{
    const char *e = getenv("CS_LEDS");
    if (e && *e && !atoi(e))
        return 0;
    const char *d = getenv("CS_LED_DIR");
    if (d && *d)
        snprintf(dir, sizeof dir, "%s", d);
    char p[320];
    snprintf(p, sizeof p, "%s/effect_rgb_hex_f1", dir);
    if (access(p, W_OK) != 0)
        return 0; /* no such LEDs here (or not ours to write) */
    FILE *sf = fopen(SAVED_FILE, "r");
    int from_file = 0;
    if (sf) { /* the last run didn't get to put them back */
        int n = 0;
        for (int z = 0; z < NZONE; z++)
            n += fscanf(sf, "%15s %7s", saved_col[z], saved_eff[z]);
        for (int i = 0; i < NSCALE; i++)
            n += fscanf(sf, "%7s", saved_scale[i]);
        n += fscanf(sf, "%7s", saved_enable);
        fclose(sf);
        from_file = n == 2 * NZONE + NSCALE + 1;
    }
    if (!from_file) {
        for (int z = 0; z < NZONE; z++) {
            char name[32];
            snprintf(name, sizeof name, "effect_rgb_hex_%s", zone_name[z]);
            get(name, saved_col[z], sizeof saved_col[z]);
            snprintf(name, sizeof name, "effect_%s", zone_name[z]);
            get(name, saved_eff[z], sizeof saved_eff[z]);
            if (!saved_col[z][0])
                strcpy(saved_col[z], "-");
            if (!saved_eff[z][0])
                strcpy(saved_eff[z], "0");
        }
        for (int i = 0; i < NSCALE; i++) {
            get(scale_name[i], saved_scale[i], sizeof saved_scale[i]);
            if (!saved_scale[i][0])
                strcpy(saved_scale[i], "-");
        }
        get("effect_enable", saved_enable, sizeof saved_enable);
        if (!saved_enable[0])
            strcpy(saved_enable, "-");
        if ((sf = fopen(SAVED_FILE, "w"))) {
            for (int z = 0; z < NZONE; z++)
                fprintf(sf, "%s %s\n", saved_col[z], saved_eff[z]);
            for (int i = 0; i < NSCALE; i++)
                fprintf(sf, "%s\n", saved_scale[i]);
            fprintf(sf, "%s\n", saved_enable);
            fclose(sf);
        }
    }
    char v[16];
    snprintf(v, sizeof v, "%d\n", LED_SCALE);
    for (int i = 0; i < NSCALE; i++)
        put(scale_name[i], v);
    put("effect_enable", "1\n");
    for (int z = 0; z < NZONE; z++) {
        char name[32];
        snprintf(name, sizeof name, "effect_cycles_%s", zone_name[z]);
        put(name, "-1\n");
    }
    memset(want, 0, sizeof want);
    memset(have_written, 0, sizeof have_written);
    on = 1;
    running = 1;
    if (pthread_create(&thread, NULL, led_thread, NULL) != 0) {
        running = 0;
        leds_flush(); /* at least dark to start with */
    }
    return 1;
}

void leds_stop(void)
{
    if (!on)
        return;
    if (running) {
        running = 0;
        pthread_join(thread, NULL);
    }
    /* back as they were: colour, then effect (which shows it), brightness */
    for (int z = 0; z < NZONE; z++) {
        char name[32];
        if (strcmp(saved_col[z], "-")) {
            snprintf(name, sizeof name, "effect_rgb_hex_%s", zone_name[z]);
            put(name, saved_col[z]);
        }
        snprintf(name, sizeof name, "effect_%s", zone_name[z]);
        put(name, saved_eff[z]);
    }
    for (int i = 0; i < NSCALE; i++)
        if (strcmp(saved_scale[i], "-"))
            put(scale_name[i], saved_scale[i]);
    if (strcmp(saved_enable, "-"))
        put("effect_enable", saved_enable);
    remove(SAVED_FILE);
    on = 0;
}

/* a colour at a brightness 0..1 */
static uint32_t scale(uint32_t c, float k)
{
    k = k < 0.f ? 0.f : k > 1.f ? 1.f : k;
    uint32_t r = (uint32_t)(((c >> 16) & 255) * k), g = (uint32_t)(((c >> 8) & 255) * k), b = (uint32_t)((c & 255) * k);
    return r << 16 | g << 8 | b;
}

static uint64_t ms_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

void leds_update(int scene_on)
{
    if (!on)
        return;
    uint32_t w[NZONE];
    w[Z_F1] = scene_on == 0 ? COL_SCENE : 0;
    w[Z_F2] = scene_on == 1 ? COL_SCENE : 0;

    /* the tempo: each beat (four 16ths) starts a flash that fades over the
     * beat, in steps so the driver isn't written every frame */
    static int last_step = -1;
    static uint64_t beat_at;
    static int bar_beat;
    uint64_t now = ms_now();
    int step = live.step;
    if (live.playing && step != last_step && step % 4 == 0) {
        beat_at = now;
        bar_beat = step % 16 == 0;
    }
    last_step = step;
    float beat_ms = 60000.f / (song.bpm > 1.f ? song.bpm : 120.f);
    float t = live.playing && beat_at ? (float)(now - beat_at) / beat_ms : 1.f;
    float k = t < 1.f ? floorf((1.f - t) * (1.f - t) * 6.f + 0.5f) / 6.f : 0.f;
    w[Z_M] = scale(bar_beat ? COL_BAR : COL_BEAT, k);

    /* the sound: the master's level, in the loudest channel's colour */
    int loud = 0;
    for (int v = 1; v < NV; v++)
        if (live.meter[v] > live.meter[loud])
            loud = v;
    float lvl = live.meter[NV];
    if (lvl > 0.98f)
        w[Z_LR] = COL_CLIP;
    else
        w[Z_LR] = scale(chan_col[loud], floorf(fminf(1.f, lvl * 1.25f) * 8.f + 0.5f) / 8.f);

    pthread_mutex_lock(&mx);
    memcpy(want, w, sizeof want);
    pthread_mutex_unlock(&mx);
}
