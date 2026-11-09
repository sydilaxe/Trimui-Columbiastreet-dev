/* TrimUI Brick, Brick Hammer and Brick Pro front-end: SDL2 as shipped with
 * MustardOS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768
 * screen), reading the buttons of MustardOS's controller. The 640x480 pages are drawn as
 * on the Miyoo and scaled 1.6x to fill the screen. As in Jaffedrone's TrimUI port: the TrimUI's joystick
 * buttons, USB audio output with hot-plug, USB MIDI clock, memory locked.
 *
 * Built twice: CS_BRICKPRO adds the Brick Pro's two analog sticks (and makes
 * the self-updater fetch the Brick Pro's package). */
#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "aclock.h"
#include "breaks.h"
#include "engine.h"
#include "gfx.h"
#include "leds.h"
#include "link.h"
#include "midi.h"
#include "perflog.h"
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
#define SET_CHOICE "breakset.txt" /* the set of breaks picked last */
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
    uint64_t t0 = perf_audio_begin(len / 4); /* the log's load and dropout counters */
    aclock_request(t0, len / 4);
    engine_render((int16_t *)stream, len / 4);
    aclock_made(len / 4, engine_position(), engine_steps_per_frame());
    perf_audio_output((const int16_t *)stream, len / 4); /* JUMP: a click made here */
    leds_audio((const short *)stream, len / 4);          /* the top bar's level meter (Brick Pro) */
    perf_audio_end(t0);
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
    perf_audio_open(rate, have.samples, name);
    /* which way the sound goes (on MustardOS: PipeWire, or ALSA through PipeWire's plugin), and what the device took */
    perf_log("AUDIO", "SDL %s driver: %d Hz, %d channels, format 0x%x, %d frames a buffer", SDL_GetCurrentAudioDriver(),
             have.freq, (int)have.channels, (unsigned)have.format, (int)have.samples);
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

/* The buttons, as SDL sees them: MustardOS's "muOS-Keys" controller,
 * numbered as in MustardOS's own controller maps (device/tui-brick*), which
 * differ between the Brick and the Brick Pro. The d-pad is hat 0, L2 and R2
 * are buttons, the Brick Pro's sticks are axes 0, 1 and 2, 3. Any other
 * controller (the TrimUI's own one underneath it) is not read, so a press
 * can't arrive twice.
 * F1 and F2 (the function buttons on the front, mute scenes M1 and M2): on
 * the Brick they are the buttons MustardOS calls L3 and R3 (it has no sticks
 * to click). MustardOS's Brick Pro sends them as 0 and 1 (its keys F1 and F2,
 * which SDL numbers first); 2 and 3 are volume down and up, left to
 * MustardOS, 4 is HOME (half time here), 16 and 17 the sticks' clicks
 * (MustardOS's input/devices/tui-brick-pro). CS_JOYMAP=F1=n,F2=n sets others
 * (log.txt names the buttons it doesn't know). */
enum { PAD_MUOS_BRICK, PAD_MUOS_BRICKPRO, PAD_KINDS };
static const char *const pad_names[PAD_KINDS] = {"MustardOS (Brick)", "MustardOS (Brick Pro)"};

/* each pad's button numbers, in the order of the BTN_ enum (ending F1, F2),
 * then L3 and R3, then HALF: half time on and off. HALF is the HOME key on
 * the Brick Pro under MustardOS (button 4, on the left under the volume
 * keys); elsewhere
 * CS_JOYMAP=HALF=n gives it a button (log.txt names the unused ones). */
#define JB_L3 BTN_COUNT
#define JB_R3 (BTN_COUNT + 1)
#define JB_HALF (BTN_COUNT + 2)
#define JB_COUNT (BTN_COUNT + 3)
static const signed char pad_buttons[PAD_KINDS][JB_COUNT] = {
#ifdef CS_BRICKPRO
    /*                     UP  DN  LT  RT  A  B  X  Y  L1  R1  L2  R2  SEL STA MENU F1  F2  L3  R3  HALF */
    [PAD_MUOS_BRICK]    = {-1, -1, -1, -1, 5, 4, 6, 7, 8,  9,  10, 11, 12, 13, 14,  15, 16, -1, -1, -1},
    [PAD_MUOS_BRICKPRO] = {-1, -1, -1, -1, 6, 5, 7, 8, 9,  10, 11, 12, 13, 14, 15,  0,  1,  16, 17, 4},
#else
    [PAD_MUOS_BRICK]    = {-1, -1, -1, -1, 5, 4, 6, 7, 8,  9,  10, 11, 12, 13, 14,  15, 16, -1, -1, -1},
    [PAD_MUOS_BRICKPRO] = {-1, -1, -1, -1, 6, 5, 7, 8, 9,  10, 11, 12, 13, 14, 15,  0,  1,  16, 17, 4},
#endif
};

#ifdef CS_BRICKPRO
/* what each pad's axes do: the Brick Pro's sticks */
enum { AX_NONE, AX_LX, AX_LY, AX_RX, AX_RY };
#define PAD_AXES 4
static const unsigned char pad_axes[PAD_KINDS][PAD_AXES] = {
    [PAD_MUOS_BRICK] = {AX_NONE},
    [PAD_MUOS_BRICKPRO] = {AX_LX, AX_LY, AX_RX, AX_RY},
};
#endif

/* joy_map[pad][SDL button]: a BTN_, JB_L3 / JB_R3, or -1 */
static int joy_map[PAD_KINDS][32];

static void joy_map_default(void)
{
    for (int p = 0; p < PAD_KINDS; p++) {
        for (int i = 0; i < 32; i++)
            joy_map[p][i] = -1;
        for (int b = 0; b < JB_COUNT; b++)
            if (pad_buttons[p][b] >= 0)
                joy_map[p][(int)pad_buttons[p][b]] = b;
    }
    /* CS_JOYMAP=A=1,B=0,...: another firmware's numbering (for every pad) */
    const char *m = getenv("CS_JOYMAP");
    static const char *const names[JB_COUNT] = {"UP", "DOWN", "LEFT",  "RIGHT", "A",    "B",  "X",  "Y",  "L1",  "R1",
                                                "L2", "R2",   "SELECT", "START", "MENU", "F1", "F2", "L3", "R3", "HALF"};
    while (m && *m) {
        char key[16];
        int num;
        if (sscanf(m, "%15[^=]=%d", key, &num) == 2 && num >= 0 && num < 32)
            for (int b = 0; b < JB_COUNT; b++)
                if (!strcasecmp(key, names[b]))
                    for (int p = 0; p < PAD_KINDS; p++) {
                        for (int i = 0; i < 32; i++)
                            if (joy_map[p][i] == b)
                                joy_map[p][i] = -1;
                        joy_map[p][num] = b;
                    }
        m = strchr(m, ',');
        if (m)
            m++;
    }
}

/* the open controllers, by SDL instance id, and which kind each is */
#define MAX_PADS 8
static struct {
    SDL_JoystickID id;
    int kind;
} pads[MAX_PADS];
static int npads;

/* vendor and product from the GUID (Linux: bytes 4-5 and 8-9, little-endian),
 * which every SDL2 has, unlike SDL_JoystickGetVendor */
static int pad_vendor(SDL_Joystick *j)
{
    SDL_JoystickGUID g = SDL_JoystickGetGUID(j);
    return g.data[4] | g.data[5] << 8;
}
static int pad_product(SDL_Joystick *j)
{
    SDL_JoystickGUID g = SDL_JoystickGetGUID(j);
    return g.data[8] | g.data[9] << 8;
}

/* MustardOS's controller: named muOS-Keys, vendor 0x756d ("mu"); product 2 on
 * the Brick and Brick Hammer, 3 on the Brick Pro. -1 for any other. */
static int pad_kind(SDL_Joystick *j)
{
    const char *name = SDL_JoystickName(j);
    if (!(name && strcasestr(name, "muos")) && pad_vendor(j) != 0x756d)
        return -1;
    switch (pad_product(j)) {
    case 2: return PAD_MUOS_BRICK;
    case 3: return PAD_MUOS_BRICKPRO;
    }
#ifdef CS_BRICKPRO
    return PAD_MUOS_BRICKPRO;
#else
    return PAD_MUOS_BRICK;
#endif
}

/* which map an event uses, or -1 to ignore it (not MustardOS's controller) */
static int event_pad(SDL_JoystickID id)
{
    for (int i = 0; i < npads; i++)
        if (pads[i].id == id)
            return pads[i].kind;
    return -1;
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
    case SDLK_F1: return BTN_F1;
    case SDLK_F2: return BTN_F2;
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

#ifdef CS_BRICKPRO
/* the side switch (above the power button) up: the groove, see switch_open */
static int groove_mode;

/* The Brick Pro's sticks: live performance, springing back when let go;
 * only the groove R2 + the left stick sets is saved (with the pattern).
 *   left stick   with the side switch down (FILTER), pushed any way: the L2
 *                low-pass, closed by hand, darker the further it goes; let
 *                go, it opens again. With the switch up (GROOVE): up locks
 *                the Euclidean cells toward breakbeat patterns, down toward
 *                the clave family, right adds ghost notes after the hits,
 *                left before them; the further, the more. With R2 held, in
 *                either position, it sets the groove kept with the pattern
 *                the same ways, which stays when let go (ui_left_stick)
 *   right stick  the punch-in effects, as R2 + the d-pad: up repeats the
 *                last 1/8, right the last 1/16, left reverses, down stops
 *                the tape
 *   L3           tap tempo (R2 + L3: no kept groove);  R3  the fill (R2 + R3:
 *                every break channel's LENGTH at 20%, again back; ui_r3)
 * Axes: pad_axes above. */
static float stick[AX_RY + 1]; /* by AX_LX .. AX_RY: -1 .. 1, centre dead zone removed */
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
    float lx = stick[AX_LX], ly = stick[AX_LY];
    /* the filter or the groove, or with R2 held the groove kept with the pattern */
    ui_left_stick(lx, -ly, groove_mode); /* up is negative from SDL */
    /* the right stick: the way it is pushed furthest, engaged past 60%,
     * let go below 40% */
    float rx = stick[AX_RX], ry = stick[AX_RY];
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
    leds_sticks(lx, -ly, rx, -ry); /* the rings point where the sticks do (up is negative from SDL) */
}
#endif

/* The side switch, above the power button (TrimUI's FN switch).
 * On the Brick Pro it picks what the left stick plays: up the GROOVE, down
 * the FILTER. On the Brick, which has no sticks, it is half time: up halves
 * the tempo, down brings back the full one (as HOME does on the Brick Pro).
 * It is GPIO 243 on both (0 up, 1 down on the Brick Pro under MustardOS;
 * MustardOS's input daemon reads the same pin on the Brick, and nothing in
 * MustardOS acts on it), read as NextUI reads it; if no one has exported it, it is exported here and given
 * back at quit. CS_SWITCH=0 leaves it alone, CS_SWITCH=/path reads another
 * file holding 0 or 1, CS_SWITCH_FLIP=1 swaps the two positions. */
#define SWITCH_GPIO "243"
#define SWITCH_PATH "/sys/class/gpio/gpio" SWITCH_GPIO "/value"
static int sw_fd = -1, sw_exported, sw_state = -1, sw_flip;

static int write_sys(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    int ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok;
}

static void switch_open(void)
{
    const char *want = getenv("CS_SWITCH");
    if (want && !strcmp(want, "0")) {
        fprintf(stderr, "switch: not read (CS_SWITCH=0)\n");
        return;
    }
    const char *path = want && *want ? want : SWITCH_PATH;
    sw_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (sw_fd < 0 && !(want && *want) && write_sys("/sys/class/gpio/export", SWITCH_GPIO)) {
        sw_exported = 1;
        write_sys("/sys/class/gpio/gpio" SWITCH_GPIO "/direction", "in");
        sw_fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    sw_flip = env_int("CS_SWITCH_FLIP", 0);
    if (sw_fd < 0)
        fprintf(stderr, "switch: %s not found, %s (CS_SWITCH=/path names another)\n", path,
#ifdef CS_BRICKPRO
                "the left stick stays on the filter"
#else
                "no half time from it"
#endif
        );
    else
        fprintf(stderr, "switch: %s%s%s\n", path, sw_exported ? " (exported here)" : "",
                sw_flip ? ", positions swapped" : "");
}

static void switch_close(void)
{
    if (sw_fd >= 0)
        close(sw_fd);
    sw_fd = -1;
    if (sw_exported)
        write_sys("/sys/class/gpio/unexport", SWITCH_GPIO);
}

#ifndef CS_BRICKPRO
static int half_want = -1; /* the switch's half time, until the UI takes it */
#endif

/* a few times a second, from the main loop */
static void switch_poll(void)
{
#ifndef CS_BRICKPRO
    if (half_want >= 0 && ui_set_half_time(half_want)) /* waits out the update prompt */
        half_want = -1;
#endif
    char c[4];
    if (sw_fd < 0 || pread(sw_fd, c, sizeof c, 0) < 1 || (c[0] != '0' && c[0] != '1'))
        return;
    int s = (c[0] == '0') ^ sw_flip; /* 0 is up */
    if (s == sw_state)
        return;
#ifdef CS_BRICKPRO
    int first = sw_state < 0;
    sw_state = groove_mode = s;
    fprintf(stderr, "switch: %s (the left stick plays the %s)\n", s ? "up" : "down", s ? "groove" : "filter");
    ui_left_mode(s, first ? NULL : s ? "LEFT STICK: GROOVE" : "LEFT STICK: FILTER");
    sticks_apply();
#else
    sw_state = s;
    fprintf(stderr, "switch: %s (%s time)\n", s ? "up" : "down", s ? "half" : "full");
    half_want = s;
    if (ui_set_half_time(s))
        half_want = -1;
#endif
}

/* the song is saved at its full tempo, half time or not; a failure is shown
 * once (until a save works again) and logged each time */
static void save_song(void)
{
    static int warned;
    ui_lock();
    float bpm = song.bpm;
    song.bpm = ui_full_bpm();
    int ok = song_save(SAVE_PATH);
    song.bpm = bpm;
    ui_unlock();
    if (ok)
        warned = 0;
    else if (!warned) {
        warned = 1;
        char msg[80];
        snprintf(msg, sizeof msg, "NOT SAVING: %s", song_error());
        ui_notice(msg);
    }
}

/* where the song and the song files live, and whether they can be written there */
static void log_save_place(void)
{
    char cwd[512];
    if (!getcwd(cwd, sizeof cwd))
        snprintf(cwd, sizeof cwd, "?");
    struct statvfs vf;
    long long free_mb = statvfs(".", &vf) == 0 ? (long long)vf.f_bavail * vf.f_frsize / (1024 * 1024) : -1;
    int songs = 0;
    for (int n = 1; n <= NSONG; n++) {
        char p[64];
        snprintf(p, sizeof p, "songs/song%02d.sav", n);
        songs += access(p, F_OK) == 0;
    }
    fprintf(stderr, "saves: in %s, %lld MB free, the folder %s, %s %s, %d song files in songs/%s\n", cwd, free_mb,
            access(".", W_OK) == 0 ? "writable" : "NOT writable", SAVE_PATH,
            access(SAVE_PATH, F_OK) == 0 ? "there" : "not there", songs,
            access("songs", F_OK) == 0 && access("songs", W_OK) != 0 ? " (NOT writable)" : "");
}

static void open_joysticks(void)
{
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_Joystick *j = SDL_JoystickOpen(i);
        if (!j)
            continue;
        SDL_JoystickID id = SDL_JoystickInstanceID(j);
        int known = 0;
        for (int k = 0; k < npads; k++)
            known |= pads[k].id == id;
        if (known) {
            SDL_JoystickClose(j); /* already open: drop the extra reference */
            continue;
        }
        int kind = pad_kind(j);
        if (npads < MAX_PADS)
            pads[npads].id = id, pads[npads].kind = kind, npads++;
        fprintf(stderr, "controller %d: %s (%04x:%04x), %d buttons, %d axes, %d hats: %s\n", i,
                SDL_JoystickName(j), pad_vendor(j), pad_product(j), SDL_JoystickNumButtons(j),
                SDL_JoystickNumAxes(j), SDL_JoystickNumHats(j), kind >= 0 ? pad_names[kind] : "not read");
    }
}

static int screen_reset; /* set by SDL's reset events: 1 the picture, 2 the renderer and picture */

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
        int pad = event_pad(e->jbutton.which);
        if (pad < 0)
            break;
        int b = e->jbutton.button, down = e->type == SDL_JOYBUTTONDOWN;
        int m = b < 32 ? joy_map[pad][b] : -1;
        if (m >= 0 && m < BTN_COUNT)
            ui_button(m, down);
        else if (m == JB_HALF) {
            if (down)
                ui_half_time();
        }
#ifdef CS_BRICKPRO
        else if (m == JB_L3) {
            if (down)
                ui_tap_tempo();
        }
        else if (m == JB_R3) {
            if (down)
                ui_r3();
        }
#endif
        else if (down) /* worth knowing on a new firmware */
            fprintf(stderr, "button %d (not used)\n", b);
        break;
    }
    case SDL_JOYHATMOTION:
        if (e->jhat.hat == 0 && event_pad(e->jhat.which) >= 0)
            on_hat(e->jhat.value);
        break;
#ifdef CS_BRICKPRO
    case SDL_JOYAXISMOTION: {
        int pad = event_pad(e->jaxis.which);
        int role = pad >= 0 && e->jaxis.axis < PAD_AXES ? pad_axes[pad][e->jaxis.axis] : AX_NONE;
        if (role != AX_NONE) {
            stick[role] = deadzone(e->jaxis.value);
            sticks_apply();
        }
        break;
    }
#endif
    case SDL_JOYDEVICEADDED:
        open_joysticks();
        break;
    case SDL_JOYDEVICEREMOVED:
        for (int i = 0; i < npads; i++)
            if (pads[i].id == e->jdevice.which) {
                pads[i] = pads[--npads];
                break;
            }
        break;
    case SDL_AUDIODEVICEADDED:
    case SDL_AUDIODEVICEREMOVED:
        if (!e->adevice.iscapture)
            audio_hotplug();
        break;
    case SDL_RENDER_TARGETS_RESET:
    case SDL_RENDER_DEVICE_RESET:
        fprintf(stderr, "screen: SDL says the %s was reset\n", e->type == SDL_RENDER_DEVICE_RESET ? "renderer" : "picture");
        screen_reset = e->type == SDL_RENDER_DEVICE_RESET ? 2 : (screen_reset ? screen_reset : 1);
        break;
    case SDL_QUIT:
        got_signal = 1;
        break;
    }
}

/* ------------------------------------------------------------- screen */

/* A display that is switched off and on (wake_screen.sh does that, as waking
 * from sleep does) can take the renderer's picture with it: SDL then reports
 * a reset, or drawing fails, and the screen stays black while the app runs
 * on. So the texture (or the renderer and texture) is made again when SDL
 * says so, when drawing fails, and once after the screen wake has had time to
 * finish (CS_SCREEN_REBUILD=0: not). Each is logged. As in Jaffedrone. */
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static SDL_Rect dst;
static int rot;
static uint32_t fb[SCREEN_W * SCREEN_H];
static int draw_fails;
static Uint32 last_rebuild;

static void make_renderer(void)
{
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren)
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
}

static void make_texture(void)
{
    tex = ren ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H)
              : NULL;
    if (tex) /* copied as it is, alpha and all (the pixels are opaque) */
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
}

static void screen_rebuild(int renderer, const char *why)
{
    if (tex)
        SDL_DestroyTexture(tex);
    tex = NULL;
    if (renderer) {
        if (ren)
            SDL_DestroyRenderer(ren);
        ren = NULL;
        make_renderer();
    }
    make_texture();
    last_rebuild = SDL_GetTicks();
    fprintf(stderr, "screen: %s made again (%s)%s%s\n", renderer ? "renderer and picture" : "picture", why,
            tex ? "" : ": failed, ", tex ? "" : SDL_GetError());
}

/* CS_SCREEN_WAKE: a command to run once the window is on show. MustardOS's
 * mux_launch.sh points it at wake_screen.sh: the screen can stay dark when an
 * app opens its window there, until waking from sleep sets the display up
 * again, so the script does what waking does. Run in the background (a double
 * fork, so nothing is left to reap), never waited for. */
#define SCREEN_WAKE_FRAME 10 /* about a third of a second of frames */
#define SCREEN_REBUILD_AFTER 75 /* frames after the wake: 2.5 s, past its second pass */
static void screen_wake(void)
{
    const char *cmd = getenv("CS_SCREEN_WAKE");
    if (!cmd || !*cmd)
        return;
    fprintf(stderr, "screen wake: %s\n", cmd);
    fflush(stderr);
    pid_t p = fork();
    if (p == 0) {
        if (fork() == 0) {
            /* a script's own path is run as a file: the app's folder on
             * MustardOS is "Columbia Street", and sh -c splits at the space */
            if (access(cmd, R_OK) == 0)
                execl("/bin/sh", "sh", cmd, (char *)NULL);
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
            _exit(127);
        }
        _exit(0);
    }
    if (p > 0)
        waitpid(p, NULL, 0);
}

static void present(void)
{
    static int frames;
    /* opaque: the page is drawn with the alpha byte left at 0, and SDL copies
     * it to the screen as it is. MustardOS's display shows the screen's
     * alpha, so a picture with alpha 0 is black there (other firmwares ignore it). */
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        fb[i] |= 0xff000000u;
    if (screen_reset) {
        screen_rebuild(screen_reset == 2, "SDL reset it");
        screen_reset = 0;
    }
    int bad = !tex || SDL_UpdateTexture(tex, NULL, fb, SCREEN_W * 4) < 0;
    if (ren) {
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        bad |= !tex || SDL_RenderCopyEx(ren, tex, NULL, &dst, rot, NULL, SDL_FLIP_NONE) < 0;
        SDL_RenderPresent(ren);
    }
    else
        bad = 1;
    if (bad) {
        if (draw_fails++ < 5)
            fprintf(stderr, "screen: drawing failed: %s\n", SDL_GetError());
        /* at most once a second: the texture first, then everything */
        if (SDL_GetTicks() - last_rebuild > 1000)
            screen_rebuild(draw_fails > 2 || !ren, "drawing failed");
    }
    if (++frames == SCREEN_WAKE_FRAME)
        screen_wake();
    /* the wake has finished (it runs twice, a second apart): the renderer is
     * made again in case the display took its picture with it */
    const char *wake = getenv("CS_SCREEN_WAKE");
    if (frames == SCREEN_WAKE_FRAME + SCREEN_REBUILD_AFTER && wake && *wake && env_int("CS_SCREEN_REBUILD", 1))
        screen_rebuild(1, "after the screen wake");
}

/* The samples load on their own thread while the main one keeps the loading
 * screen on show: a frame drawn once can be lost on its way to the screen
 * (the first after a window opens often is), and the load takes seconds on
 * the A53s. Nothing else runs yet, so the loader has the engine to itself. */
typedef struct {
    volatile int stage; /* 0 breaks, 1 basses, 2 done */
    int nb, ns;
} Loader;

static void *load_samples(void *arg)
{
    Loader *ld = arg;
    breaks_scan_sets(BREAK_DIR);
    breaks_set_choice_file(SET_CHOICE);
    /* the set the last song was saved with, so its breaks are found by name;
     * else the one picked last */
    char want[SET_NAME] = "";
    int si = song_file_set(SAVE_PATH, want, sizeof want) && want[0] ? breaks_find_set(want) : -1;
    ld->nb = si >= 0 ? breaks_load_set(si) : breaks_load_chosen();
    __sync_synchronize();
    ld->stage = 1;
    ld->ns = bass_load_dir(BASS_DIR);
    __sync_synchronize();
    ld->stage = 2;
    return NULL;
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
    uint64_t t_start = now_us();
    fprintf(stderr, "columbia street v%s for %s\n", CS_VERSION, DEVICE_NAME);
#ifdef CS_BRICKPRO
    perf_init("CS", "brickpro"); /* the performance log, in log.txt (CS_LOG) */
#else
    perf_init("CS", "brick");
#endif

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
    win = SDL_CreateWindow("Columbia Street", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, sw, sh,
                           SDL_WINDOW_SHOWN | (env_int("CS_WINDOWED", 0) ? 0 : SDL_WINDOW_FULLSCREEN));
    if (win)
        make_renderer();
    make_texture();
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

    uint64_t t_window = now_us();
    engine_init(rate);
    Loader ld = {0};
    pthread_t loader;
    if (pthread_create(&loader, NULL, load_samples, &ld) == 0) {
        while (ld.stage < 2) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) /* controllers are opened after: only a quit counts now */
                if (e.type == SDL_QUIT)
                    got_signal = 1;
            ui_draw_loading(fb, ld.stage ? "LOADING BASSES..." : "LOADING BREAKS...");
            present();
            SDL_Delay(30);
        }
        pthread_join(loader, NULL);
    }
    else {
        ui_draw_loading(fb, "LOADING...");
        present();
        load_samples(&ld);
    }
    int nb = ld.nb, ns = ld.ns;
    fprintf(stderr, "startup: window after %.2f s, samples loaded in %.2f s\n", (t_window - t_start) / 1e6,
            (now_us() - t_window) / 1e6);
    if (!nbanks) {
        breaks_fallback();
        fprintf(stderr, "no breaks in %s/: using the fallback kit\n", BREAK_DIR);
    }
    fprintf(stderr, "%d break banks, %d bass samples\n", nb, ns);
    song_default();
    log_save_place();
    if (song_load(SAVE_PATH))
        fprintf(stderr, "restored %s\n", SAVE_PATH);
    else if (access(SAVE_PATH, F_OK) == 0) {
        char msg[80];
        snprintf(msg, sizeof msg, "LAST SONG WON'T LOAD: %s", song_error());
        ui_notice(msg);
    }

    joy_map_default();
    open_joysticks();
    switch_open();

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
#ifndef CS_BRICKPRO
    ui_l2_sticks(1); /* no sticks: L2 + the d-pad, L2 + A B X Y and R2 + the d-pad play them */
#endif
#ifdef CS_BRICKPRO
    fprintf(stderr, "leds: %s\n", leds_start() ? "F1/F2 show the mute scenes, the top bar the level, the shoulders the beat, "
                                                 "each ring its stick (CS_LED_FRAME=0: the named zones)"
                                                : "not driven (no led_anim, or CS_LEDS=0)");
#else
    fprintf(stderr, "leds: %s\n", leds_start() ? "F1/F2 show the mute scenes, the top bar the beat, lr the level"
                                                : "not driven (no led_anim, or CS_LEDS=0)");
#endif
    sync_init();
    /* the clock out leaves as the sound is heard, the device's buffer behind the engine */
    midi_set_latency((float)bufsz / rate);
    if (env_int("CS_MIDI", 1)) {
        midi_start(-1);
        /* the bottom port as a clock link: a Brick with a hub can lead this one (link.h) */
        link_follow_start();
    }
    int update_auto = env_int("CS_UPDATE", 1) != 0, update_tries = 0;
    if (update_auto) {
        net_init(CA_BUNDLE);
        ui_set_update(update_status(), install_update);
        fprintf(stderr, "updates from %s\n", update_asset());
    }
    fprintf(stderr, "update check %s\n", update_auto ? "on" : "off");
    fprintf(stderr, "startup: running after %.2f s\n", (now_us() - t_start) / 1e6);
    Uint32 last = SDL_GetTicks(), last_save = last, update_next = last + 5000, relaunch_at = 0, next_draw = last;
    while (!ui_quit_requested() && !got_signal) {
        SDL_Event e;
        while (SDL_PollEvent(&e))
            on_event(&e);
        char note[48];
        while (midi_take_notice(note, sizeof note))
            ui_notice(note);
        int mch, mcc, mv;
        while (midi_take_cc(&mch, &mcc, &mv))
            ui_midi_cc(mch, mcc, mv);
        Uint32 now = SDL_GetTicks();
        ui_tick((int)(now - last));
        last = now;
        /* the buttons are read every few milliseconds, the screen drawn 30 times a second */
        if ((Sint32)(now - next_draw) < 0) {
            SDL_Delay(2);
            continue;
        }
        sync_poll(now_us());
        static int sw_frame;
        if (sw_frame++ % 3 == 0) /* the side switch, 10 times a second */
            switch_poll();
        if (update_auto)
            update_watch(now, &update_next, &update_tries);
        if (update_status()->state == UP_INSTALLED && !relaunch_at) {
            fprintf(stderr, "update: v%s installed, relaunching\n", update_status()->latest);
            relaunch_at = now + 1200; /* long enough to read "restarting" */
        }
        if (relaunch_at && (Sint32)(now - relaunch_at) >= 0)
            break;
        uint64_t f0 = now_us();
        ui_draw(fb);
        leds_update(ui_scene_on());
        present();
        perf_frame(now_us() - f0);
        perf_tick();

        if (now - last_save > 15000) {
            last_save = now;
            if (ui_take_changed())
                save_song();
        }
        Uint32 spent = SDL_GetTicks() - now;
        next_draw = now + (spent + 2 < 33 ? 33 : spent + 2);
        SDL_Delay(2);
    }

    save_song();
    switch_close();
    leds_stop();
    link_follow_stop(); /* the bottom port given back */
    midi_stop();
    sync_close();
    if (dev)
        SDL_CloseAudioDevice(dev);
    SDL_Quit();
    breaks_free();
    perf_close();
    return update_status()->state == UP_INSTALLED ? RELAUNCH_STATUS : 0; /* launch.sh starts the new version */
}
