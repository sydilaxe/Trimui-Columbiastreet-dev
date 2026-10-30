/* The TrimUI Brick's and Brick Pro's LEDs, through the led_anim driver
 * (/sys/class/led_anim), as NextUI drives them:
 *
 *   F1, F2      lit while mute scene M1 / M2 is on
 *   top bar     the tempo: a flash on each beat, brighter and orange on the
 *               bar's first, fading through the beat; dark when stopped
 *   lr          the sound: the Brick Pro's analog sticks (the Brick's
 *               triggers) follow the output's level, in the colour of the
 *               loudest channel, red when it clips
 *
 * Writing to the driver happens on a thread of its own, at most every 25 ms
 * and only when a zone changes, so neither the sound nor the screen waits
 * for it. The settings found at start (colours, effects, brightness) are put
 * back at quit; until then leds_saved.txt in the app folder keeps them, so a
 * crash doesn't lose them (the next run puts them back). CS_LEDS=0 leaves
 * the LEDs alone; CS_LED_DIR points at another folder (for the tests).
 * Nothing happens where the driver isn't. */
#pragma once

int leds_start(void); /* 1 if the LEDs are driven */
void leds_stop(void); /* the settings found at start, back */
/* main thread, each frame: work out what the LEDs should show now (from the
 * engine's live state, the song and the scene that is on) */
void leds_update(int scene_on);
/* the tests: write what is due now, on this thread */
void leds_flush(void);
