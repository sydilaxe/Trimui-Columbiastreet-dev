/* Columbia Street audio engine: four break channels that play slices of
 * sampled drum breaks and one bass channel, each sequenced by three
 * polymetric Euclidean cells (HIT, ROLL, ALT). Per break channel: pitch,
 * filter, bit depth (1-24), sample-rate reduction and a compressor. On the
 * bus: reverb, a glue compressor, drive, the L2 filter sweep and punch-in
 * repeat / reverse / tape-stop effects. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "breaks.h" /* SET_NAME */

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
    /* the groove's feel, kept with the pattern and each slot as the lock and
     * ghosts are: swing 0..25 (50% straight to 75%), breath 0..100 (the
     * beat's dynamics and lean, engine_step_gain) */
    int8_t swing, breath;
    int8_t slot_feel[NSLOT][2];
    /* v0.6: MIDI clock over USB (MIDI_*), the device's like the Wi-Fi sync mode;
     * midi_clock_only: follow the clock's tempo (and over the Brick link, its
     * bar) but not START, STOP, CONTINUE or song position */
    uint8_t midi_mode, midi_clock_only;
    /* v0.10: the set of breaks the song was saved with (a folder in breaks/),
     * brought in when it loads; empty in older songs, which keep the set in */
    char break_set[SET_NAME];
    /* v0.11: half time on when the song was saved (its bpm is still the full
     * tempo); a song switching in brings it back, older songs at full time */
    uint8_t half_time;
} Song;

/* the MIDI clock: AUTO sends it and follows one that comes in, SEND always
 * sends it and ignores one coming in, FOLLOW only follows, OFF neither (notes
 * and CCs still come in). AUTO is 0, what older songs have */
enum { MIDI_AUTO, MIDI_SEND, MIDI_FOLLOW, MIDI_OFF, NMIDIMODE };

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
     * live.step), fill_to 0 when there's none; the channels playing its
     * snare and its kick (the same one when no other plays a kick), and
     * which of the fill shapes it plays */
    int fill_from, fill_to, fill_snare, fill_kick, fill_shape;
    /* the one-touch remix, the same way: steps remix_from .. remix_to - 1,
     * remix_to 0 when there's none; the channel it plays on (its snare), the
     * step of its one roll (-1 none), its seed, and the flams and ghost
     * notes it has played so far */
    int remix_from, remix_to, remix_snare, remix_roll, remix_seed, remix_flams, remix_ghosts;
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
/* the one-touch fill, from the next 16th to the end of the bar (on its last
 * 16th, the whole next bar). A drummer's fill on the pattern's own kick and
 * snare, one of several shapes (a lead-in over beats 1 and 2, the fill over 3
 * and 4), never the same twice running; the break running in GRID drops out.
 * 1 armed, -1 a fill already armed or playing called off, 0 stopped. Under the lock. */
int engine_fill(void);
/* the one-touch remix, armed and called off as the fill is (the two never
 * at once: arming one calls the other off): the pattern plays on, its snare
 * channel funked up with a tasteful few flams, ghost notes and at most one
 * short roll into the bar line, different each time. 1 armed, -1 called
 * off, 0 stopped. Under the lock. */
int engine_remix(void);
/* fill shape i over the bar's 16ths (16 characters): K kick,
 * S snare, g ghost snare, f flammed snare, r a 32nd pair on the snare, . rest */
const char *engine_fill_shape(int i);
int engine_fill_shapes(void);
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
float engine_step_gain(int step); /* BREATH's dynamics for a step: 1 at 0 */
void euclid_permute(Cell *c);
void song_default(void);
void pattern_generate(Pattern *p, unsigned seed); /* a fresh jungle pattern */
void song_clamp(void); /* after the banks change, keep params in range */
/* another set of breaks swapped into banks[] (audio locked): the hits playing
 * from the old samples stop, and the params come into range */
void engine_banks_changed(void);
int song_save(const char *path);
int song_load(const char *path);

void slot_save(int i);
void slot_load(int i); /* now when stopped, at the next switch_div when playing */
/* song files: the whole set (slots, grooves, mixer, tempo, scenes; not the
 * output, sync or clock settings, which belong to the device). song_file_load
 * switches it in as slot_load does; 0 if the file is missing or not a song */
int song_file_save(const char *path, float bpm);
int song_file_load(const char *path);
/* why the last save or load failed, in capitals for the screen ("THE SD CARD IS FULL");
 * the details go to the log */
const char *song_error(void);
/* the folder for the song files, made if it isn't there; 0 (and song_error says
 * why: THE SD CARD IS READ-ONLY...) if it can't be */
int song_folder_ready(const char *dir);
/* the set of breaks a song file was saved with, into out ("" for a song from
 * before sets); 0 if the file won't read */
int song_file_set(const char *path, char *out, int n);
void slot_clear(int i);
