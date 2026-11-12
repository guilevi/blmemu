/* keys_term.c -- see keys_term.h. */
#include <ctype.h>
#include <string.h>
#include "keys_term.h"

static int seq_key(const unsigned char *b, int n, int *used)   /* an escape sequence at b: its key, or 0 */
{
    int i, num = 0;
    *used = 0;
    if (n < 2) return -1;                                       /* incomplete */
    if (b[1] != '[' && b[1] != 'O') { *used = 1; return KT_ESC; }
    for (i = 2; i < n; i++) {
        unsigned char c = b[i];
        if (isdigit(c)) { num = num * 10 + (c - '0'); continue; }
        if (c == ';') { num = 0; continue; }
        *used = i + 1;
        switch (c) {
        case 'A': return KT_UP;
        case 'B': return KT_DOWN;
        case 'C': return KT_RIGHT;
        case 'D': return KT_LEFT;
        case '~': return num == 23 ? KT_F11 : num == 24 ? KT_F12 : KT_NONE;
        default: return KT_NONE;
        }
    }
    return -1;
}

int kt_decode(kt_dec *d, const unsigned char *bytes, int n, int *out, int max)
{
    unsigned char b[80];
    int len = 0, i = 0, k = 0;
    memcpy(b, d->held, (size_t)d->n_held);
    len = d->n_held;
    if (n > (int)sizeof b - len) n = (int)sizeof b - len;
    memcpy(b + len, bytes, (size_t)n);
    len += n;
    d->n_held = 0;
    while (i < len && k < max) {
        unsigned char c = b[i];
        if (c == 0x1B) {
            int used, key = seq_key(b + i, len - i, &used);
            if (key < 0) {
                if (n == 0 || len - i > 12) { out[k++] = KT_ESC; i++; continue; }   /* nothing more coming */
                memcpy(d->held, b + i, (size_t)(len - i));
                d->n_held = len - i;
                break;
            }
            if (key) out[k++] = key;
            i += used;
            continue;
        }
        i++;
        if (c == 0x0D || c == 0x0A) out[k++] = KT_ENTER;
        else if (c == 0x7F || c == 0x08) out[k++] = KT_BACKSPACE;
        else if (c == 0x09) out[k++] = KT_TAB;
        else if (c >= 1 && c <= 26) out[k++] = KT_CTRL + 'a' + c - 1;
        else if (c >= 32 && c < 127) out[k++] = KT_CHAR + c;
    }
    return k;
}

void kt_map_default(kt_map *m)
{
    memcpy(m->dot, "fdsjkla;", 8);
    m->space = ' ';
}

int kt_dot(const kt_map *m, int ch)
{
    int i;
    ch = tolower(ch);
    for (i = 0; i < 8; i++)
        if (tolower((unsigned char)m->dot[i]) == ch) return 1 << i;
    return 0;
}

int kt_chord_key(kt_chord *c, const kt_map *m, int key, double t)
{
    int ch, d;
    if (key < KT_CHAR || key >= KT_CHAR + 127) return 0;
    ch = key - KT_CHAR;
    d = kt_dot(m, ch);
    if (!d && ch != m->space) return 0;
    if (key == c->last_key && t - c->last_key_t < KT_REPEAT_MS / 1000.0) {
        c->last_key_t = t;                                    /* the terminal repeating a key held down */
        return 1;
    }
    c->last_key = key;
    c->last_key_t = t;
    if (d) c->dots |= d;
    else c->space = 1;
    c->n++;
    c->last = t;
    return 1;
}

int kt_chord_due(kt_chord *c, double t, int *dots, int *space)
{
    if (!c->n || t - c->last < KT_CHORD_MS / 1000.0) return 0;
    *dots = c->dots;
    *space = c->space;
    c->dots = c->space = c->n = 0;
    return 1;
}
