/* TrimUI Brick, Brick Hammer and Brick Pro front-end: SDL2 as shipped with
 * TrimUI OS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768
 * screen). The 640x480 pages are drawn as on the Miyoo and scaled 1.6x to
 * fill the screen. As in Jaffedrone's TrimUI port: the TrimUI's joystick
 * buttons, USB audio output with hot-plug, USB MIDI clock, memory locked.
 *
 * Built twice: CS_BRICKPRO adds the Brick Pro's two analog sticks (and makes
 * the self-updater fetch the Brick Pro's zip). */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "aclock.h"
#include "breaks.h"
#include "engine.h"
#include "gfx.h"
#include "midi.h"
#include "net.h"
#include "sync.h"
#include "ui.h"
#include "update.h"

#ifdef CS_BRICKPRO
#define DEVICE_NAME "TrimUI Brick Pro"
#else
#define DEVICE_NAME "TrimUI Brick"
#endif

#define SAVE_PATH "columbia.sav"
#define BREAK_DIR "breaks"
#define BASS_DIR "bass"
#define CA_BUNDLE "cacert.pem"

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static volatile sig_atomic_t got_signal;
static void on_signal(int s) { got_signal = s; }

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

/* ------------------------------------------------------------- audio */

static SDL_AudioDeviceID dev;
static char dev_name[128]; /* "" is the system's own output */
static int rate, bufsz;
/* the UI's and MIDI's edits against the audio callback; dev_mx keeps the
 * device from being swapped (USB audio plugged in) while one is held */
static pthread_mutex_t dev_mx = PTHREAD_MUTEX_INITIALIZER;
static void lock(void)
{
    pthread_mutex_lock(&dev_mx);
    if (dev)
        SDL_LockAudioDevice(dev);
}
static void unlock(void)
{
    if (dev)
        SDL_UnlockAudioDevice(dev);
    pthread_mutex_unlock(&dev_mx);
}

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    aclock_request(now_us(), len / 4);
    engine_render((int16_t *)stream, len / 4);
    aclock_made(len / 4, engine_position(), engine_steps_per_frame());
}

static int is_usb_name(const char *n)
{
    return n && (strcasestr(n, "usb") || strcasestr(n, "uac") || strcasestr(n, "dac"));
}

/* CS_AUDIO picks the output: "auto" (a USB device when one is plugged in,
 * else the speaker and headphones), "internal", or part of a device's name */
static const char *pick_output(void)
{
    const char *want = getenv("CS_AUDIO");
    if (want && !strcasecmp(want, "internal"))
        return NULL;
    int n = SDL_GetNumAudioDevices(0);
    for (int i = 0; i < n; i++) {
        const char *name = SDL_GetAudioDeviceName(i, 0);
        if (!name)
            continue;
        if (want && *want && strcasecmp(want, "auto") ? strcasestr(name, want) != NULL : is_usb_name(name))
            return name;
    }
    return NULL;
}

/* the device opens paused; start: let it run at once (a device that comes
 * later, while the sequencer is already playing) */
static int open_output_locked(const char *name, int start);
static int open_output(const char *name, int start)
{
    pthread_mutex_lock(&dev_mx);
    int ok = open_output_locked(name, start);
    pthread_mutex_unlock(&dev_mx);
    return ok;
}

static int open_output_locked(const char *name, int start)
{
    if (dev) {
        SDL_CloseAudioDevice(dev);
        dev = 0;
    }
    /* the driver holds about a buffer before it's heard; CS_SYNC_LATENCY
     * (frames) lines this unit up with other apps by ear if needed. Each
     * device starts the audio clock afresh. */
    aclock_init(rate, env_int("CS_SYNC_LATENCY", bufsz));
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)bufsz;
    want.callback = audio_cb;
    /* the engine always makes 16-bit stereo at its own rate: SDL converts
     * for a device that wants something else */
    dev = SDL_OpenAudioDevice(name, 0, &want, &have, 0);
    if (!dev && name) {
        fprintf(stderr, "audio: could not open %s (%s), using the speaker\n", name, SDL_GetError());
        name = NULL;
        dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    }
    if (!dev) {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
        return 0;
    }
    snprintf(dev_name, sizeof dev_name, "%s", name ? name : "");
    fprintf(stderr, "audio out: %s, %d Hz, %d frames a buffer\n", name ? name : "speaker / headphones", rate, bufsz);
    if (start)
        SDL_PauseAudioDevice(dev, 0);
    return 1;
}

/* a USB device came or went: follow it (unless CS_AUDIO names one) */
static void audio_hotplug(void)
{
    const char *want = getenv("CS_AUDIO");
    if (want && *want && strcasecmp(want, "auto"))
        return;
    const char *name = pick_output();
    if (!strcmp(name ? name : "", dev_name))
        return;
    open_output(name, 1);
    char msg[48];
    snprintf(msg, sizeof msg, "AUDIO: %.38s", name ? name : "SPEAKER");
    ui_notice(msg);
}

/* ------------------------------------------------------------- input */

/* the TrimUI's buttons, as SDL sees them (TrimUI OS, NextUI and MinUI
 * agree): the d-pad is hat 0, L2 and R2 are axes 2 and 5 */
enum { JB_B = 0, JB_A = 1, JB_Y = 2, JB_X = 3, JB_L1 = 4, JB_R1 = 5, JB_SELECT = 6, JB_START = 7, JB_MENU = 8,
       JB_L3 = 9, JB_R3 = 10 };
static int joy_map[32];

static void joy_map_default(void)
{
    for (int i = 0; i < 32; i++)
        joy_map[i] = -1;
    joy_map[JB_B] = BTN_B, joy_map[JB_A] = BTN_A, joy_map[JB_Y] = BTN_Y, joy_map[JB_X] = BTN_X;
    joy_map[JB_L1] = BTN_L1, joy_map[JB_R1] = BTN_R1;
    joy_map[JB_SELECT] = BTN_SELECT, joy_map[JB_START] = BTN_START, joy_map[JB_MENU] = BTN_MENU;
    /* CS_JOYMAP=A=1,B=0,...: another firmware's numbering */
    const char *m = getenv("CS_JOYMAP");
    static const char *const names[BTN_COUNT] = {"UP", "DOWN", "LEFT", "RIGHT", "A", "B", "X", "Y",
                                                  "L1", "R1", "L2", "R2", "SELECT", "START", "MENU"};
    while (m && *m) {
        char key[16];
        int num;
        if (sscanf(m, "%15[^=]=%d", key, &num) == 2 && num >= 0 && num < 32)
            for (int b = 0; b < BTN_COUNT; b++)
                if (!strcasecmp(key, names[b])) {
                    for (int i = 0; i < 32; i++)
                        if (joy_map[i] == b)
                            joy_map[i] = -1;
                    joy_map[num] = b;
                }
        m = strchr(m, ',');
        if (m)
            m++;
    }
}

static int map_key(SDL_Keycode k)
{
    switch (k) { /* a keyboard, for trying it on a computer */
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;
    case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_SPACE: return BTN_A;
    case SDLK_LCTRL: return BTN_B;
    case SDLK_LSHIFT: return BTN_X;
    case SDLK_LALT: return BTN_Y;
    case SDLK_e: return BTN_L1;
    case SDLK_t: return BTN_R1;
    case SDLK_TAB: return BTN_L2;
    case SDLK_BACKSPACE: return BTN_R2;
    case SDLK_RCTRL: return BTN_SELECT;
    case SDLK_RETURN: return BTN_START;
    case SDLK_ESCAPE: return BTN_MENU;
    default: return -1;
    }
}

static int hat_was;
static void on_hat(int v)
{
    static const int bits[4] = {SDL_HAT_UP, SDL_HAT_DOWN, SDL_HAT_LEFT, SDL_HAT_RIGHT};
    static const int btn[4] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
    for (int i = 0; i < 4; i++)
        if ((v ^ hat_was) & bits[i])
            ui_button(btn[i], (v & bits[i]) != 0);
    hat_was = v;
}

/* L2 and R2 are axes: pressed past halfway, let go below a quarter */
static int trig_down[2];
static void on_trigger(int which, int value)
{
    int down = trig_down[which] ? value > 8192 : value > 16384;
    if (down != trig_down[which]) {
        trig_down[which] = down;
        ui_button(which ? BTN_R2 : BTN_L2, down);
    }
}

#ifdef CS_BRICKPRO
/* The Brick Pro's sticks: live performance, springing back when let go;
 * nothing they do is saved.
 *   left stick   pushed any way: the L2 low-pass, closed by hand, darker the
 *                further it goes; let go, it opens again
 *   right stick  the punch-in effects, as R2 + the d-pad: up repeats the
 *                last 1/8, right the last 1/16, left reverses, down stops
 *                the tape
 *   L3           tap tempo;  R3  a new beat (as SELECT)
 * Axes (as on the TrimUI Smart Pro): 0, 1 left x, y; 3, 4 right x, y. */
#define AXIS_LX 0
#define AXIS_LY 1
#define AXIS_RX 3
#define AXIS_RY 4
static float stick[5]; /* -1 .. 1, centre dead zone removed */
static int stick_fx_now = -1;

static float deadzone(int v)
{
    float x = v / 32767.f, dz = 0.12f;
    if (x > -dz && x < dz)
        return 0.f;
    x = x > 0.f ? (x - dz) / (1.f - dz) : (x + dz) / (1.f - dz);
    return x > 1.f ? 1.f : x < -1.f ? -1.f : x;
}

static void sticks_apply(void)
{
    float lx = stick[AXIS_LX], ly = stick[AXIS_LY];
    float amount = sqrtf(lx * lx + ly * ly);
    ui_lock();
    live.sweep_hand = amount > 1.f ? 1.f : amount;
    ui_unlock();
    /* the right stick: the way it is pushed furthest, engaged past 60%,
     * let go below 40% */
    float rx = stick[AXIS_RX], ry = stick[AXIS_RY];
    float ax = fabsf(rx), ay = fabsf(ry), m = ax > ay ? ax : ay;
    int fx = stick_fx_now;
    if (m < 0.4f)
        fx = -1;
    else if (m > 0.6f)
        fx = ay >= ax ? (ry < 0.f ? FX_REPEAT8 : FX_TAPESTOP) : (rx > 0.f ? FX_REPEAT16 : FX_REVERSE);
    if (fx != stick_fx_now) {
        stick_fx_now = fx;
        ui_stick_fx(fx);
    }
}
#endif

static void open_joysticks(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_Joystick *j = SDL_JoystickOpen(i);
        if (j)
            fprintf(stderr, "controller %d: %s, %d buttons, %d axes, %d hats\n", i, SDL_JoystickName(j),
                    SDL_JoystickNumButtons(j), SDL_JoystickNumAxes(j), SDL_JoystickNumHats(j));
    }
}

static void on_event(const SDL_Event *e)
{
    switch (e->type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (!e->key.repeat)
            ui_button(map_key(e->key.keysym.sym), e->type == SDL_KEYDOWN);
        break;
    case SDL_JOYBUTTONDOWN:
    case SDL_JOYBUTTONUP: {
        int b = e->jbutton.button, down = e->type == SDL_JOYBUTTONDOWN;
        int m = b < 32 ? joy_map[b] : -1;
        if (m >= 0)
            ui_button(m, down);
#ifdef CS_BRICKPRO
        else if (b == JB_L3 && down)
            ui_tap_tempo();
        else if (b == JB_R3 && down)
            ui_button(BTN_SELECT, 1), ui_button(BTN_SELECT, 0);
#endif
        else if (down) /* worth knowing on a new firmware */
            fprintf(stderr, "button %d (not used)\n", b);
        break;
    }
    case SDL_JOYHATMOTION:
        if (e->jhat.hat == 0)
            on_hat(e->jhat.value);
        break;
    case SDL_JOYAXISMOTION:
        if (e->jaxis.axis == 2)
            on_trigger(0, e->jaxis.value);
        else if (e->jaxis.axis == 5)
            on_trigger(1, e->jaxis.value);
#ifdef CS_BRICKPRO
        else if (e->jaxis.axis < 5) {
            stick[e->jaxis.axis] = deadzone(e->jaxis.value);
            sticks_apply();
        }
#endif
        break;
    case SDL_JOYDEVICEADDED:
        open_joysticks();
        break;
    case SDL_AUDIODEVICEADDED:
    case SDL_AUDIODEVICEREMOVED:
        if (!e->adevice.iscapture)
            audio_hotplug();
        break;
    case SDL_QUIT:
        got_signal = 1;
        break;
    }
}

/* ------------------------------------------------------------- screen */

static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_Rect dst;
static int rot;
static uint32_t fb[SCREEN_W * SCREEN_H];

static void present(void)
{
    SDL_UpdateTexture(tex, NULL, fb, SCREEN_W * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderCopyEx(ren, tex, NULL, &dst, rot, NULL, SDL_FLIP_NONE);
    SDL_RenderPresent(ren);
}

/* ------------------------------------------------------------- update */

static void install_update(void) { update_install_start("."); }

/* Ask GitHub for the latest release 5 s after starting (so Wi-Fi is up), then
 * every minute until it answers, five tries in all. */
static void update_watch(Uint32 now, Uint32 *next_check, int *tries)
{
    static int logged;
    UpdateStatus *u = update_status();
    if ((u->state == UP_CURRENT || u->state == UP_AVAILABLE) && !logged) {
        logged = 1;
        fprintf(stderr, "update: the latest release is v%s, this is v%s\n", u->latest, CS_VERSION);
    }
    if ((u->state == UP_IDLE || u->state == UP_CHECK_FAILED) && *tries < 5 && (Sint32)(now - *next_check) >= 0) {
        if (u->state == UP_CHECK_FAILED)
            fprintf(stderr, "%s\n", u->err);
        ++*tries;
        *next_check = now + 60000;
        update_check_start();
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    srand((unsigned)time(NULL));
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    fprintf(stderr, "columbia street v%s for %s\n", CS_VERSION, DEVICE_NAME);

    rate = env_int("CS_RATE", 48000);
    bufsz = env_int("CS_BUFFER", 512); /* the A53s have the time: half the Miyoo's delay */

    /* 1 GB: the whole program and its samples stay in memory, so the sound
     * never waits for a page to be read back in */
    if (env_int("CS_MLOCK", 1) && mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "could not lock memory\n");

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, getenv("CS_SCALE") ? getenv("CS_SCALE") : "linear");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    SDL_DisplayMode dm;
    int sw = 1024, sh = 768;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0)
        sw = dm.w, sh = dm.h;
    SDL_Window *win = SDL_CreateWindow("Columbia Street", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, sw, sh,
                                       SDL_WINDOW_SHOWN | (env_int("CS_WINDOWED", 0) ? 0 : SDL_WINDOW_FULLSCREEN));
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED) : NULL;
    if (win && !ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H)
              : NULL;
    if (!tex) {
        fprintf(stderr, "video: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    {
        SDL_RendererInfo ri;
        SDL_GetRendererInfo(ren, &ri);
        SDL_GetRendererOutputSize(ren, &sw, &sh);
        fprintf(stderr, "%s: screen %dx%d, drawn by %s, %d cores\n", DEVICE_NAME, sw, sh, ri.name,
                (int)sysconf(_SC_NPROCESSORS_ONLN));
    }
    /* CS_ROTATE=90/180/270, for a firmware whose screen is turned */
    rot = env_int("CS_ROTATE", 0);
    int side = rot == 90 || rot == 270;
    float scale = side ? fminf((float)sw / SCREEN_H, (float)sh / SCREEN_W) : fminf((float)sw / SCREEN_W, (float)sh / SCREEN_H);
    dst.w = (int)(SCREEN_W * scale + 0.5f), dst.h = (int)(SCREEN_H * scale + 0.5f);
    dst.x = (sw - dst.w) / 2, dst.y = (sh - dst.h) / 2;

    ui_draw_loading(fb, "LOADING BREAKS...");
    present();

    engine_init(rate);
    int nb = breaks_load_dir(BREAK_DIR);
    int ns = bass_load_dir(BASS_DIR);
    if (!nbanks) {
        breaks_fallback();
        fprintf(stderr, "no breaks in %s/: using the fallback kit\n", BREAK_DIR);
    }
    fprintf(stderr, "%d break banks, %d bass samples\n", nb, ns);
    song_default();
    if (song_load(SAVE_PATH))
        fprintf(stderr, "restored %s\n", SAVE_PATH);

    joy_map_default();
    open_joysticks();

    int have_audio = open_output(pick_output(), 0);
    if (have_audio) {
        ui_lock = lock;
        ui_unlock = unlock;
        SDL_PauseAudioDevice(dev, 0);
    }
    else
        fprintf(stderr, "no audio: %s\n", SDL_GetError());
    /* the screen below the sound */
    if (setpriority(PRIO_PROCESS, 0, 10) != 0)
        fprintf(stderr, "could not lower the screen's priority\n");

    ui_init();
    sync_init();
    /* the clock out leaves as the sound is heard, the device's buffer behind the engine */
    midi_set_latency((float)bufsz / rate);
    if (env_int("CS_MIDI", 1))
        midi_start(-1);
    int update_auto = env_int("CS_UPDATE", 1) != 0, update_tries = 0;
    if (update_auto) {
        net_init(CA_BUNDLE);
        ui_set_update(update_status(), install_update);
    }
    fprintf(stderr, "update check %s\n", update_auto ? "on" : "off");
    Uint32 last = SDL_GetTicks(), last_save = last, update_next = last + 5000, relaunch_at = 0, next_draw = last;
    while (!ui_quit_requested() && !got_signal) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            on_event(&e);
        char note[48];
        while (midi_take_notice(note, sizeof note))
            ui_notice(note);
        Uint32 now = SDL_GetTicks();
        ui_tick((int)(now - last));
        last = now;
        /* the buttons are read every few milliseconds, the screen drawn 30 times a second */
        if ((Sint32)(now - next_draw) < 0) {
            SDL_Delay(2);
            continue;
        }
        sync_poll(now_us());
        if (update_auto)
            update_watch(now, &update_next, &update_tries);
        if (update_status()->state == UP_INSTALLED && !relaunch_at) {
            fprintf(stderr, "update: v%s installed, relaunching\n", update_status()->latest);
            relaunch_at = now + 1200; /* long enough to read "restarting" */
        }
        if (relaunch_at && (Sint32)(now - relaunch_at) >= 0)
            break;
        ui_draw(fb);
        present();

        if (now - last_save > 15000) {
            last_save = now;
            if (ui_take_changed())
                song_save(SAVE_PATH);
        }
        Uint32 spent = SDL_GetTicks() - now;
        next_draw = now + (spent + 2 < 33 ? 33 : spent + 2);
        SDL_Delay(2);
    }

    song_save(SAVE_PATH);
    midi_stop();
    sync_close();
    if (dev)
        SDL_CloseAudioDevice(dev);
    SDL_Quit();
    breaks_free();
    return update_status()->state == UP_INSTALLED ? RELAUNCH_STATUS : 0; /* launch.sh starts the new version */
}
