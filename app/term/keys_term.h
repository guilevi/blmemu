/* keys_term.h -- a terminal's bytes as keys, and the Millennium's chords made from them.
 *
 * A terminal says when a key is pressed (and repeats it while held), never when it comes up.  So a chord is the keys
 * typed close together: it goes to the unit KT_CHORD_MS after the last one, and a key repeated within KT_REPEAT_MS
 * of itself is the terminal's auto-repeat, not a new press.
 */
#ifndef BLM_KEYS_TERM_H
#define BLM_KEYS_TERM_H

enum {
    KT_NONE = 0,
    KT_CHAR = 0x100,             /* + the character (1-126) */
    KT_ENTER = 0x200, KT_BACKSPACE, KT_TAB, KT_ESC, KT_UP, KT_DOWN, KT_LEFT, KT_RIGHT, KT_F11, KT_F12, KT_CTRL
                                 /* KT_CTRL + 'a'..'z': Ctrl and a letter */
};

#define KT_CHORD_MS 80
#define KT_REPEAT_MS 150

/* The keys in bytes (n of them, a read's worth): up to max keys into out.  An escape sequence split across reads is
   held in d until the rest comes. */
typedef struct {
    unsigned char held[16];
    int n_held;
} kt_dec;
int kt_decode(kt_dec *d, const unsigned char *bytes, int n, int *out, int max);

/* The braille keys: a character's dot (bit 0 = dot 1 .. bit 7 = dot 8), 0 if it is not one; the space bar is
   kt_space.  From the settings ([keys]: dot1 = f, ...). */
typedef struct {
    char dot[8];                 /* the character for each dot */
    char space;
} kt_map;
void kt_map_default(kt_map *m);  /* F D S J K L, A for dot 7, ; for dot 8 (the unit's 7 3 2 1 4 5 6 8, in order) */
int kt_dot(const kt_map *m, int ch);

typedef struct {
    int dots, space, n;
    double last;                 /* when the chord's last key came */
    int last_key;
    double last_key_t;
} kt_chord;
/* A key typed at time t (seconds): 1 when it was a braille key (and is now part of the chord). */
int kt_chord_key(kt_chord *c, const kt_map *m, int key, double t);
/* At time t: 1 and the chord in *dots, *space when it is complete. */
int kt_chord_due(kt_chord *c, double t, int *dots, int *space);

#endif
