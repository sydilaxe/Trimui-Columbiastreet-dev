/* Break banks and bass samples, loaded from the app folder.
 *
 * The breaks come in sets, one at a time: each folder in breaks/ is a set
 * (breaks/DIRT/ the curated one), and .wav files loose in breaks/ make one
 * more, BREAKS. A bank is one mono loop with slice points at its hits. A
 * .wav file in a set is a whole loop, cut by onset detection; a tempo in its
 * name ("apache_115bpm.wav") puts the slices on the 16th grid. A folder in a
 * set is a loop that is already chopped: its .wav files, in name order, are
 * the slices. bass/ holds one-shots the bass channel can play pitched. */
#pragma once
#include <stdint.h>

#define MAX_BANKS 32
#define MAX_SLICES 64
#define MAX_BASS 32
#define NPEAK 256
#define MAX_SETS 64
#define SET_NAME 40
#define CURATED_SET "DIRT"  /* the curated breaks, fetched by tools/fetch_samples.sh */
#define LOOSE_SET "BREAKS"  /* the .wav files straight in breaks/ */

/* what a slice starts with, guessed from its spectrum (for the generator) */
enum { TAG_KICK = 1, TAG_SNARE = 2, TAG_HAT = 4 };

typedef struct {
    char name[8];
    float *data;   /* mono, at the engine rate */
    int len;
    float bpm;     /* native tempo */
    int steps;     /* 16th notes the loop spans */
    int nslices;
    int start[MAX_SLICES + 1]; /* sample offsets; start[nslices] == len */
    float at[MAX_SLICES];      /* each slice's position in 16ths */
    uint8_t tag[MAX_SLICES];
    float peak[NPEAK];         /* overview for the waveform display */
} Bank;

typedef struct {
    char name[8];
    float *data;
    int len;
    float root_hz; /* detected pitch, 0 if none was found */
} BassSample;

extern Bank banks[MAX_BANKS];
extern int nbanks;
extern BassSample bass_smp[MAX_BASS];
extern int nbass;

void breaks_set_rate(int sample_rate);
int breaks_load_dir(const char *dir); /* one set's folder into banks[]; returns how many banks were added */

/* the sets in root (breaks/), in name order with BREAKS first; tidies away the
 * old copies of the curated breaks an update leaves outside DIRT/ */
int breaks_scan_sets(const char *root);
int breaks_nsets(void);
const char *breaks_set_name(int i);
int breaks_find_set(const char *name); /* -1 if there is none */
int breaks_set_current(void);          /* the set in banks[], or -1 */
/* the file that remembers the set picked last (breakset.txt); none by default */
void breaks_set_choice_file(const char *path);
int breaks_load_set(int i);  /* at start-up, nothing playing yet: into banks[] */
int breaks_load_chosen(void); /* the one remembered, else DIRT, else the first */

/* switching while the music plays: start loading set i on a thread (0 if one
 * is loading already); once the state is READY, commit swaps it into banks[]
 * with the audio locked (1; -1 if nothing in it loaded, and banks[] is
 * untouched), and cleanup, unlocked, frees the old samples and remembers the
 * set */
enum { SWITCH_IDLE, SWITCH_LOADING, SWITCH_READY };
int breaks_switch_start(int i);
int breaks_switch_state(void);
int breaks_switch_target(void); /* the set loading, or -1 */
int breaks_switch_commit(void);
void breaks_switch_cleanup(void);

int bass_load_dir(const char *dir);
void breaks_fallback(void);            /* a small drum-machine bank when no samples are installed */
void breaks_free(void);

int bank_slice_at(const Bank *b, double step); /* the slice sounding at a grid position */
int bank_slice_exact(const Bank *b, double step); /* a slice starts on this step */
int bank_slice_end(const Bank *b, int i);      /* GRID end of slice i */
int bank_find_tag(const Bank *b, int tag, int nth); /* nth slice with the tag, or -1 */
int bank_count_tag(const Bank *b, int tag);

/* exposed for the test harness */
int bank_from_loop(Bank *b, const char *name, float *data, int len, float bpm);
