/* session.h -- one Braille Lite Millennium in use: the unit, its DoubleTalk, its memory on disk.  What every shell
 * (the terminal's, the Windows window's) drives; the shells only add keys, sound and a menu.
 *
 * The firmware and the memory are in the data folder (fwstore.h).  A unit with no memory yet is set up as it left
 * the factory: the hard reset (I-chord held at power-on), every question answered yes, until it says it is ready;
 * then switched off and on.  Quitting switches it off, as a person would -- the firmware saves what it keeps and
 * stops -- and the memory is saved; a minute's save also runs while it is on, in case the program does not get to
 * quit.
 *
 * Thread-safe: the sound thread renders while another thread presses keys.
 */
#ifndef BLM_SESSION_H
#define BLM_SESSION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct blm_session blm_session;

/* What the session tells the person (setting up, a save that failed); ctx is the shell's */
typedef void (*session_note_fn)(void *ctx, const char *text);

/* Opens the unit from the data folder's firmware and memory, setting it up first when there is no memory (a few
   seconds).  NULL with a reason a person can read. */
blm_session *session_open(session_note_fn note, void *ctx, char *err, size_t errlen);
void session_close(blm_session *s);                   /* switched off, saved, freed */

/* Sound: n samples at `rate` Hz, the unit run for exactly that long.  The rate may change between calls (the device
   changed). */
void session_render(blm_session *s, int16_t *out, size_t n, unsigned rate);
/* Time without sound (a silent run): the unit run for `seconds`, its speech unheard. */
void session_run_silent(blm_session *s, double seconds);

/* Keys.  A chord pressed and let go; keys down now (held until changed: a window's keys down and up). */
void session_chord(blm_session *s, int dots, int space);
void session_keys(blm_session *s, int dots, int space);

/* Switched off and on, its files kept; with keys held through the start when dots or space are given (they stay
   down until session_keys lets them go).  I-chord is the hard reset. */
void session_restart(blm_session *s, int hold_dots, int hold_space);
/* A new unit: every file gone, set up as from the factory (as session_open does the first time). */
int session_factory(blm_session *s, char *err, size_t errlen);

void session_display_mode(blm_session *s, int on);   /* the Braille Display Mode switch */
int session_display_mode_on(blm_session *s);

/* The braille display: its cells (dot 1 = bit 0 .. dot 8 = bit 7, left to right) and a count that changes whenever
   they do.  Returns the number of cells (40). */
int session_cells(blm_session *s, uint8_t *cells, int cap, unsigned *changes);

/* The text sent to the DoubleTalk, a line at a time as it is spoken (for a log or a test); NULL to stop. */
void session_on_speech(blm_session *s, void (*fn)(void *ctx, const char *line), void *ctx);

int session_save(blm_session *s, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
