/* Columbia Street audio engine: four break channels that play slices of
 * sampled drum breaks and one bass channel, each sequenced by three
 * polymetric Euclidean cells (HIT, ROLL, ALT). Per break channel: pitch,
 * filter, bit depth (1-24), sample-rate reduction and a compressor. On the
 * bus: reverb, a glue compressor, drive, the L2 filter sweep and punch-in
 * repeat / reverse / tape-stop effects. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* the app offers any release newer than this. Release builds set it from the tag
 * (make VERSION=x.y.z, which CI does); this is the version of a plain build. */
#ifndef CS_VERSION
#define CS_VERSION "0.6.0"
#endif

#define NV 5    /* channels */
#define NBRK 4  /* break channels 0..3; channel 4 is the bass */
#define V_BASS 4
#define NROW 3
#define NP 13   /* params per channel */
#define MAX_STEPS 32
#define NSLOT 10 /* pattern memory A..J */
#define NSLOT_V4 8 /* A..H, where they were saved up to v0.4; I and J come after */
#define NSONG 16 /* song files 1..16 in songs/ */
#define NFXSLOT 4

enum { ROW_HIT, ROW_ROLL, ROW_ALT };
enum { TRIG_HIT = 1, TRIG_ROLL = 2, TRIG_ALT = 4 };

/* break channel params */
enum {
    B_BANK, B_SLICE, B_MODE, B_RANGE, B_PITCH, B_FIT, B_LEN,
    B_ROLL, B_ALT, B_TONE, B_BITS, B_RATE, B_COMP
};
/* how a hit picks its slice */
enum { MODE_FIX, MODE_WALK, MODE_GRID, MODE_RAND, NMODE };
/* what the ALT cell does to a hit */
enum { ALT_REV, ALT_OCTUP, ALT_OCTDN, ALT_NEXT, ALT_RAND, ALT_STOP, NALT };

/* bass params */
enum {
    S_WAVE, S_ROOT, S_OCT, S_RIFF, S_LEN, S_GLIDE, S_CUT, S_RES,
    S_WIDTH, S_DRIVE, S_ROLL, S_ALT
};
#define NBASS_P 12
enum { WAVE_SUB, WAVE_REESE, WAVE_808, WAVE_GROWL, NWAVE_SYNTH }; /* then the bass/ samples */
enum { BALT_OCTUP, BALT_FIFTH, BALT_OCTDN, BALT_SLIDE, BALT_REST, NBALT };

enum { SYNC_OFF, SYNC_AUTO, SYNC_LEAD, SYNC_FOLLOW, NSYNCMODE };
/* the output, as on HI5: stereo, or the sound in mono on one side and clock
 * pulses on the other (clk_side 0 = clock on the right), clk_ppqn pulses per
 * quarter note (an index into 1, 2, 4, 8, 16, 24, 32), clk_width ms wide.
 * In mono + clock every channel plays from the centre (the pans are kept). */
enum { OUT_STEREO, OUT_MONO_CLOCK };
#define NPPQN 7
/* punch-in effects on R2 + up / right / left / down */
enum { FX_REPEAT8, FX_REPEAT16, FX_REVERSE, FX_TAPESTOP };

typedef struct {
    uint8_t on, steps, hits, rot;
} Cell;

typedef struct {
    const char *name;
    int min, max, def;
} ParamDef;

#define NSCENE 2 /* mute scenes M1, M2 */

/* one pattern: what the slots A..J store and recall */
typedef struct {
    Cell cell[NV][NROW];
    int param[NV][NP];
    uint8_t mute[NV];
} Pattern;

/* Everything the user edits; saved to disk as-is. */
typedef struct {
    uint32_t magic, size;
    float bpm;
    Pattern pat;
    int vol[NV], pan[NV], send[NV];
    uint8_t solo[NV];
    int master, glue, master_drive, verb_time, verb_tone, sweep_rate;
    int sync_mode;
    Pattern slot[NSLOT_V4]; /* A..H; song_slot(i) reaches all ten */
    uint8_t slot_used[NSLOT_V4];
    /* v0.2: new fields go at the end, so an older save still loads */
    int out_mode, clk_side, clk_ppqn, clk_width;
    /* v0.4: two mute scenes, the mixer's mutes and solos saved to switch on
     * and off in one press (F1 / F2 on the TrimUI, R2 + L1 / R1 anywhere) */
    uint8_t scene_mute[NSCENE][NV], scene_solo[NSCENE][NV], scene_used[NSCENE];
    /* v0.5: slots I and J; the groove kept with the pattern and with each slot
     * (-100..100, as live.groove_lock / groove_ghost below, which override it
     * while the stick or R2 + the d-pad plays); when a slot or song switches in */
    Pattern slot_more[NSLOT - NSLOT_V4];
    uint8_t slot_more_used[NSLOT - NSLOT_V4];
    int8_t groove_lock, groove_ghost;
    int8_t slot_groove[NSLOT][2];
    uint8_t switch_div; /* SW_* */
} Song;

/* when a queued slot or song switches in */
enum { SW_BEAT, SW_HALF, SW_BAR, SW_2BARS, SW_4BARS, NSWITCH };
int switch_steps(int div); /* in 16ths: 4, 8, 16, 32, 64 */
const char *switch_name(int div);

Pattern *song_slot(Song *s, int i); /* slot i of A..J */
uint8_t *song_slot_used(Song *s, int i);

/* Shared between UI and audio thread. The UI writes these only while
 * holding the audio lock; the audio thread publishes the rest. */
typedef struct {
    int playing;
    int filter_held;
    float sweep_hand;      /* the Brick Pro's left stick: 0 open .. 1 the L2 sweep closed */
    /* the groove played live (the side switch up: the left stick, or a set
     * amount on the Brick), never saved; 0 leaves the groove kept in the song
     * (song.groove_lock / groove_ghost, in hundredths), and either at 0 changes nothing.
     *   groove_lock   > 0 pulls the HIT cells toward breakbeat patterns, < 0
     *                 toward the clave family; 1 / -1 plays the pattern
     *   groove_ghost  quiet ghost notes on the empty 16ths: > 0 after a hit,
     *                 < 0 before one; the size is how many (from 0.6 they
     *                 reach two 16ths from the hit and the 8th offbeats) */
    float groove_lock, groove_ghost;
    int fx;                /* punch-in effect held, or -1 */
    int queued_slot;       /* pattern slot switching in at the next song.switch_div, or -1 */
    const Song *queued_song; /* a song switching in the same way, or NULL */
    /* the one-touch fill: steps fill_from .. fill_to - 1 (counted as
     * live.step), fill_to 0 when there's none; fill_v the channel that rolls */
    int fill_from, fill_to, fill_v;
    volatile uint32_t songs_loaded; /* counts the songs switched in */
    int cur_slot;          /* slot last loaded or saved, or -1 */
    volatile int step;
    volatile uint32_t trig_count[NV];
    volatile int slice[NBRK];   /* slice each break channel last played */
    volatile float play_pos[NBRK]; /* playhead, 0..1 of the bank, -1 when silent */
    volatile int note;          /* MIDI note the bass last played */
    volatile float meter[NV + 1];
    volatile float gr[NV + 1];  /* compressor gain reduction in dB (bass slot unused, last = glue) */
    volatile float cutoff_norm;
} Live;

extern Song song;
extern Live live;
extern const char *const voice_names[NV];

const ParamDef *param_def(int v, int p);
int nparam(int v);
int param_min(int v, int p);
int param_max(int v, int p);
void param_format(int v, int p, char *buf, int n);
void param_set(int v, int p, int x); /* clamps */
const char *riff_name(int i);
int riff_len(int i);
int riff_note(int i, int k);
const char *sweep_rate_name(int i);
int clk_ppqn_value(int i); /* clk_ppqn index -> pulses per quarter note */
int nsweep(void);

void engine_init(int sample_rate);
void engine_render(int16_t *out, int frames);
void engine_audition(int v);
/* a MIDI note for channel ch (0-3 a break's slices, 4 the bass), velocity 0
 * a note off; any thread. Played at the start of the next block. */
void engine_note(int ch, int note, int vel);
void engine_set_playing(int on);
/* the one-touch fill, to the end of the bar: the second half of it, from the
 * next 16th once that has begun, or of the next bar on its last 16th. Ghost
 * notes all over beat 3, then the snare channel rolls up into the bar line.
 * 1 armed, -1 a fill already armed or playing called off, 0 stopped. Under the lock. */
int engine_fill(void);
/* transport position in 16th steps with fraction, a hard jump, and a
 * playback speed trim for the Wi-Fi sync (1.0 = none); all under the lock */
double engine_position(void);
void engine_set_position(double steps);
void engine_set_rate(double r);
double engine_steps_per_frame(void); /* how far the position moves each frame */

int euclid_hit(const Cell *c, int step);
/* the groove's view of a step: TRIG_ flags after the lock, and the velocity
 * of the ghost note there, or 0 for none (channel v, any step) */
int groove_flags(int v, int step);
float groove_ghost_at(int v, int step);
float groove_lock_now(void);  /* live.groove_lock, or the kept one: -1..1 */
float groove_ghost_now(void);
void euclid_permute(Cell *c);
void song_default(void);
void pattern_generate(Pattern *p, unsigned seed); /* a fresh jungle pattern */
void song_clamp(void); /* after the banks change, keep params in range */
int song_save(const char *path);
int song_load(const char *path);

void slot_save(int i);
void slot_load(int i); /* now when stopped, at the next switch_div when playing */
/* song files: the whole set (slots, grooves, mixer, tempo, scenes; not the
 * output, sync or clock settings, which belong to the device). song_file_load
 * switches it in as slot_load does; 0 if the file is missing or not a song */
int song_file_save(const char *path, float bpm);
int song_file_load(const char *path);
void slot_clear(int i);
