/* Pages, controls and drawing. Platform code feeds button edges and time. */
#pragma once
#include <stdint.h>
#include <stdio.h>

enum {
    BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_A, BTN_B, BTN_X, BTN_Y,
    BTN_L1, BTN_R1, BTN_L2, BTN_R2,
    BTN_SELECT, BTN_START, BTN_MENU,
    BTN_F1, BTN_F2, /* the TrimUI's function buttons: mute scenes M1 and M2 (R2 + L1 / R1 elsewhere) */
    BTN_COUNT
};

enum { PAGE_PATTERN, PAGE_CHOP, PAGE_BASS, PAGE_MIX, PAGE_SONG, PAGE_COUNT };

/* Set by the platform to serialise UI edits against the audio callback. */
extern void (*ui_lock)(void);
extern void (*ui_unlock)(void);

/* The self-update prompt (src/update.h): the platform passes the updater's status
 * and a function that starts the install. Without it the prompt never shows. */
struct UpdateStatus;
void ui_set_update(const struct UpdateStatus *status, void (*install)(void));

void ui_init(void);
void ui_button(int btn, int down);
void ui_tick(int ms);
void ui_draw(uint32_t *fb);
void ui_draw_loading(uint32_t *fb, const char *msg);
int ui_quit_requested(void);
int ui_take_changed(void);
int ui_page(void);
void ui_notice(const char *msg); /* a message on screen (main thread only): USB devices coming and going */
/* MIDI CC: the important settings, by CC number (any channel); 1 if it was
 * one. Main thread. ui_midi_chart writes the CC table of the MIDI chart. */
int ui_midi_cc(int ch, int cc, int v);
void ui_midi_chart(FILE *f);
/* the Brick Pro's sticks: a punch-in effect (FX_*) while the right stick is
 * pushed, or -1; and a tap of the tempo (L3) */
void ui_stick_fx(int fx);
void ui_tap_tempo(void);
/* half time: the tempo halved on one press, back to what it was on the next
 * (the HOME key on the Brick Pro under MustardOS); ui_full_bpm is the tempo
 * to save, the full one while half time is on */
void ui_half_time(void);
float ui_full_bpm(void);
/* the side switch: what the left stick plays (1 the groove, 0 the filter),
 * shown in the header; msg, if not NULL, on screen for a moment */
void ui_left_mode(int groove, const char *msg);
void ui_set_page(int p); /* for the test harness */
int ui_scene_on(void);    /* the mute scene switched on (0 M1, 1 M2), or -1 */
