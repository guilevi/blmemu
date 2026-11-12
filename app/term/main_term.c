/* main_term.c -- the Braille Lite Millennium 40 in a terminal (macOS and Linux): the session (session.h), the
 * keyboard, sound at the output device's own rate, and a menu.  What the program itself says is plain lines of text,
 * for the screen reader to read.
 *
 *   blmemu [--no-sound] [--braille] [--text] [--home DIR] [--import-firmware PATH] [--import-doubletalk PATH]
 *
 * Keys: F D S J K L = dots 1 2 3 4 5 6, A = dot 7, ; = dot 8, the space bar; typed close together they are one
 * chord.  F11 or Ctrl+O opens the menu.  The rest: Help in the menu, or README.md.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "audio.h"
#include "keys_term.h"
#include "fwstore.h"
#include "session.h"
#include "settings.h"

#ifndef BLMEMU_VERSION
#define BLMEMU_VERSION "dev"
#endif

#define AHEAD_S 0.06                      /* sound queued ahead of the device */
#define SLICE_S 0.005
#define HARD_RESET_HOLD_S 1.5             /* I-chord held this long into the start */

static struct termios saved_tio;
static int raw_on, tty;

static void raw_off(void)
{
    if (raw_on) tcsetattr(0, TCSANOW, &saved_tio);
    raw_on = 0;
}

static void raw_mode(void)
{
    struct termios t;
    if (!tty || raw_on) return;
    t = saved_tio;
    t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG | IEXTEN);
    t.c_iflag &= ~(tcflag_t)(IXON | ICRNL);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSANOW, &t);
    raw_on = 1;
}

static void say(const char *s)            /* a line of the program's own, for the screen reader */
{
    fputs(s, stdout);
    fputs(raw_on ? "\r\n" : "\n", stdout);
    fflush(stdout);
}

static void sayf(const char *fmt, const char *a)
{
    char b[1024];
    snprintf(b, sizeof b, fmt, a);
    say(b);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static void note(void *ctx, const char *t) { (void)ctx; say(t); }

/* ---- the firmware, the first time ------------------------------------------------------------------------------- */
static int ask_path(const char *question, char *out, size_t cap)
{
    raw_off();
    printf("%s\n> ", question);
    fflush(stdout);
    if (!fgets(out, (int)cap, stdin)) return -1;
    out[strcspn(out, "\r\n")] = 0;
    fw_clean_path(out);
    return out[0] ? 0 : -1;
}

static int bring_in(int which, const char *given)
{
    char path[FW_PATH_MAX], what[256], err[512];
    for (;;) {
        if (given) snprintf(path, sizeof path, "%s", given);
        else if (ask_path(which == 0
                              ? "The Millennium's own firmware: type or drag in BLM40ENU.BNS (the Millennium 40 update "
                                "file from Freedom Scientific), then Enter. Enter alone gives up."
                              : "The DoubleTalk's firmware: type or drag in RC Studio's folder (RC Systems' free "
                                "software; its Bin folder holds the firmware), or an RC8650 .srec image, then Enter. "
                                "Enter alone gives up.",
                          path, sizeof path))
            return -1;
        if ((which == 0 ? fw_import_bns : fw_import_rc8650)(path, what, sizeof what, err, sizeof err) == 0) {
            sayf("Taken: %s.", what);
            return 0;
        }
        sayf("That did not work: %s.", err);
        if (given) return -1;
    }
}

/* ---- the menu ------------------------------------------------------------------------------------------------------- */
enum { M_OFF, M_MENU, M_FACTORY, M_PATH_BNS, M_PATH_RC };

typedef struct {
    blm_session *s;
    settings *ini;
    kt_map map;
    int sound, braille, text, quit, reopen;
    int mode;                             /* M_*: what the line being typed is for */
    char line[FW_PATH_MAX];
    int n_line;
    double release_at;                    /* the hard reset's I-chord comes up then */
    int hold_next, holding, held_dots, held_space;
} app;

static void menu_list(app *a)
{
    say("Menu: type a number and Enter. Enter alone goes back to the unit.");
    say(" 1  Restart the unit: switched off and on, its files kept");
    say(" 2  Hard reset: restart with I-chord held. The unit asks its questions: answer them with y or n");
    say(" 3  Back to the factory state: erases this unit's files (asks first)");
    say(session_display_mode_on(a->s) ? " 4  Braille Display Mode switch: on. Turn it off (a note-taker again)"
                                      : " 4  Braille Display Mode switch: off. Turn it on (a braille display for a PC)");
    say(a->braille ? " 5  Show the braille display on each change: on. Turn it off"
                   : " 5  Show the braille display on each change: off. Turn it on");
    say(a->text ? " 6  Show what the unit says: on. Turn it off" : " 6  Show what the unit says: off. Turn it on");
    say(" 7  Bring in the Millennium's firmware again");
    say(" 8  Bring in the DoubleTalk's firmware again");
    say(" 9  The keys");
    say(" a  About");
    say(" 0  Exit: the unit is switched off and its memory saved");
}

static void keys_help(void)
{
    say("Keys: F D S J K L are dots 1 2 3 4 5 6, A is dot 7, ; is dot 8 (the unit's own order, left to right: "
        "7 3 2 1 4 5 6 8), and the space bar. Keys typed together are one chord.");
    say("Enter is dot 8, Backspace dot 7. The arrows are the dot-1, dot-4, dot-3 and dot-6 chords.");
    say("F11 or Ctrl+O: the menu. Ctrl+B: show the braille display. Ctrl+T: show what the unit says. "
        "Ctrl+D: the Braille Display Mode switch. Ctrl+K: hold the next chord down (Ctrl+K again lets it go). "
        "Ctrl+R: restart the unit. Ctrl+Q: exit.");
    say("The keys can be changed in blmemu.ini in the data folder, section [keys]: dot1 = f ... dot8 = ;");
}

static void about(app *a)
{
    char home[FW_PATH_MAX], b[1200];
    say("Braille Lite Millennium emulator " BLMEMU_VERSION ". Freedom Scientific's firmware on an emulated Z180, "
        "its DoubleTalk on librc8650 (RC Systems' firmware on an emulated M16C). GPL-3.");
    if (fw_home(home, sizeof home) == 0) sayf("Its data folder: %s", home);
    if (a->sound) {
        snprintf(b, sizeof b, "Sound at %u Hz, the output device's own rate.", audio_rate());
        say(b);
    } else
        say("Running silent.");
}

static void set_bool(app *a, const char *key, int *v, int on, const char *yes, const char *no)
{
    *v = on;
    settings_set_int(a->ini, "display", key, on);
    settings_save(a->ini);
    say(on ? yes : no);
}

static void toggle_display_mode(app *a)
{
    int on = !session_display_mode_on(a->s);
    session_display_mode(a->s, on);
    settings_set_int(a->ini, "unit", "display_mode", on);
    settings_save(a->ini);
    say(on ? "Braille Display Mode switch: on (toward the front)." : "Braille Display Mode switch: off (a note-taker).");
}

static void restart(app *a, int hard)
{
    if (hard) {
        a->holding = 0;
        say("Restarting with I-chord held: answer the unit's questions with y or n.");
        session_restart(a->s, 0x0A, 1);
        a->release_at = now_s() + HARD_RESET_HOLD_S;
    } else {
        say(a->holding ? "Switching off and on, the chord held." : "Switching off and on.");
        session_restart(a->s, a->holding ? a->held_dots : 0, a->holding ? a->held_space : 0);
    }
}

static void line_done(app *a)
{
    char *l = a->line, err[512];
    int mode = a->mode;
    a->line[a->n_line] = 0;
    a->n_line = 0;
    a->mode = M_OFF;
    if (mode == M_FACTORY) {
        if (strcmp(l, "yes")) { say("Nothing erased."); return; }
        if (session_factory(a->s, err, sizeof err)) sayf("%s", err);
        return;
    }
    if (mode == M_PATH_BNS || mode == M_PATH_RC) {
        char what[256];
        fw_clean_path(l);
        if (!*l) { say("Nothing changed."); return; }
        if ((mode == M_PATH_BNS ? fw_import_bns : fw_import_rc8650)(l, what, sizeof what, err, sizeof err)) {
            sayf("That did not work: %s.", err);
            return;
        }
        sayf("Taken: %s. The unit restarts with it.", what);
        a->reopen = 1;
        return;
    }
    if (!*l) { say("Back to the unit."); return; }
    switch (l[0]) {
    case '1': restart(a, 0); break;
    case '2': restart(a, 1); break;
    case '3':
        say("Erase every file on this unit and set it up as new? Type yes and Enter; anything else keeps it.");
        a->mode = M_FACTORY;
        break;
    case '4': toggle_display_mode(a); break;
    case '5': set_bool(a, "braille", &a->braille, !a->braille, "The braille display is shown on each change.",
                       "The braille display is not shown."); break;
    case '6': set_bool(a, "text", &a->text, !a->text, "What the unit says is shown.", "What the unit says is not shown.");
        break;
    case '7':
        say("Type or drag in the Millennium 40 update file (BLM40ENU.BNS), then Enter. Enter alone keeps the one in use.");
        a->mode = M_PATH_BNS;
        break;
    case '8':
        say("Type or drag in RC Studio's folder or an RC8650 .srec, then Enter. Enter alone keeps the one in use.");
        a->mode = M_PATH_RC;
        break;
    case '9': keys_help(); break;
    case 'a': case 'A': about(a); break;
    case '0': a->quit = 1; break;
    case '?': menu_list(a); a->mode = M_MENU; break;
    default: say("Not a choice. ? lists them."); a->mode = M_MENU;
    }
}

/* a key while a line is being typed (the menu, a path, yes) */
static void line_key(app *a, int k)
{
    if (k == KT_ENTER) { say(""); line_done(a); return; }
    if (k == KT_ESC || k == KT_F11 || k == KT_CTRL + 'o') { a->n_line = 0; a->mode = M_OFF; say("Back to the unit."); return; }
    if (k == KT_BACKSPACE) {
        if (a->n_line) { a->n_line--; fputs("\b \b", stdout); fflush(stdout); }
        return;
    }
    if (k >= KT_CHAR && k < KT_CHAR + 127 && a->n_line < (int)sizeof a->line - 1) {
        a->line[a->n_line++] = (char)(k - KT_CHAR);
        putchar(k - KT_CHAR);              /* echoed, so the screen reader says it */
        fflush(stdout);
    }
}

static void chord(app *a, int dots, int space)
{
    if (a->holding) {                      /* the chord held down comes up */
        session_keys(a->s, 0, 0);
        a->holding = 0;
    }
    if (a->hold_next) {
        a->hold_next = 0;
        a->holding = 1;
        a->held_dots = dots;
        a->held_space = space;
        session_keys(a->s, dots, space);
        say("Holding that chord down: Ctrl+K lets it go.");
        return;
    }
    session_chord(a->s, dots, space);
}

static void unit_key(app *a, kt_chord *c, int k, double t)
{
    switch (k) {
    case KT_F11: case KT_CTRL + 'o': menu_list(a); a->mode = M_MENU; return;
    case KT_CTRL + 'q': a->quit = 1; return;
    case KT_CTRL + 'r': restart(a, 0); return;
    case KT_CTRL + 'b':
        set_bool(a, "braille", &a->braille, !a->braille, "The braille display is shown on each change.",
                 "The braille display is not shown.");
        return;
    case KT_CTRL + 't':
        set_bool(a, "text", &a->text, !a->text, "What the unit says is shown.", "What the unit says is not shown.");
        return;
    case KT_CTRL + 'd': toggle_display_mode(a); return;
    case KT_CTRL + 'k':
        if (a->holding) { session_keys(a->s, 0, 0); a->holding = 0; say("Let go."); }
        else { a->hold_next = 1; say("The next chord is held down."); }
        return;
    case KT_ENTER: chord(a, 0x80, 0); return;
    case KT_BACKSPACE: chord(a, 0x40, 0); return;
    case KT_UP: chord(a, 0x01, 1); return;
    case KT_DOWN: chord(a, 0x08, 1); return;
    case KT_LEFT: chord(a, 0x04, 1); return;
    case KT_RIGHT: chord(a, 0x20, 1); return;
    }
    kt_chord_key(c, &a->map, k, t);
}

static void speech_line(void *ctx, const char *line)
{
    app *a = (app *)ctx;
    if (a->text) say(line);
}

static void load_keys(app *a)
{
    static const char *const names[8] = {"dot1", "dot2", "dot3", "dot4", "dot5", "dot6", "dot7", "dot8"};
    int i;
    kt_map_default(&a->map);
    for (i = 0; i < 8; i++) {
        const char *v = settings_get(a->ini, "keys", names[i], NULL);
        if (v && v[0]) a->map.dot[i] = !strcmp(v, "semicolon") ? ';' : !strcmp(v, "comma") ? ',' :
                                       !strcmp(v, "period") ? '.' : !strcmp(v, "slash") ? '/' : v[0];
    }
}

static void usage(void)
{
    printf("blmemu %s -- the Braille Lite Millennium 40, on its own firmware.\n"
           "  --no-sound                  run silent\n"
           "  --braille                   show the braille display from the start\n"
           "  --text                      show what the unit says\n"
           "  --home DIR                  keep the firmware, memory and settings in DIR\n"
           "  --import-firmware PATH      bring in BLM40ENU.BNS without asking\n"
           "  --import-doubletalk PATH    bring in RC Studio's folder or an RC8650 .srec without asking\n",
           BLMEMU_VERSION);
}

int main(int argc, char **argv)
{
    app a;
    const char *imp_bns = NULL, *imp_rc = NULL;
    char err[512];
    int i;
    memset(&a, 0, sizeof a);
    a.sound = 1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-sound")) a.sound = 0;
        else if (!strcmp(argv[i], "--braille")) a.braille = 2;
        else if (!strcmp(argv[i], "--text")) a.text = 2;
        else if (!strcmp(argv[i], "--home") && i + 1 < argc) setenv("BLMEMU_HOME", argv[++i], 1);
        else if (!strcmp(argv[i], "--import-firmware") && i + 1 < argc) imp_bns = argv[++i];
        else if (!strcmp(argv[i], "--import-doubletalk") && i + 1 < argc) imp_rc = argv[++i];
        else if (!strcmp(argv[i], "--version")) { printf("blmemu %s\n", BLMEMU_VERSION); return 0; }
        else { usage(); return strcmp(argv[i], "--help") ? 2 : 0; }
    }
    tty = isatty(0) && tcgetattr(0, &saved_tio) == 0;
    atexit(raw_off);

    printf("Braille Lite Millennium emulator %s.\n", BLMEMU_VERSION);
    {
        char p[FW_PATH_MAX];
        FILE *f;
        if (imp_bns && bring_in(0, imp_bns)) return 1;
        if (imp_rc && bring_in(1, imp_rc)) return 1;
        if (fw_path(FW_BNS_NAME, p, sizeof p) || !(f = fopen(p, "rb"))) {
            if (!tty || bring_in(0, NULL)) { say("The Millennium's firmware is needed to start: see README.md."); return 1; }
        } else fclose(f);
        if (fw_path(FW_RC_NAME, p, sizeof p) || !(f = fopen(p, "rb"))) {
            if (!tty || bring_in(1, NULL)) { say("The DoubleTalk's firmware is needed to start: see README.md."); return 1; }
        } else fclose(f);
    }

    a.ini = settings_load();
    if (a.braille != 2) a.braille = settings_int(a.ini, "display", "braille", 0);
    if (a.text != 2) a.text = settings_int(a.ini, "display", "text", 0);
    a.braille = a.braille != 0;
    a.text = a.text != 0;
    load_keys(&a);

open_unit:
    a.s = session_open(note, NULL, err, sizeof err);
    if (!a.s) { sayf("The unit cannot start: %s.", err); return 1; }
    session_display_mode(a.s, settings_int(a.ini, "unit", "display_mode", 0));
    session_on_speech(a.s, speech_line, &a);
    if (a.sound && !audio_rate() && audio_open(err, sizeof err)) {
        sayf("%s; running silent.", err);
        a.sound = 0;
    }
    if (!a.reopen) {
        say("Braille Lite Millennium 40. F11 or Ctrl+O: the menu. Ctrl+Q: exit.");
        if (a.sound) {
            snprintf(err, sizeof err, "Sound at %u Hz.", audio_rate());
            say(err);
        }
    }
    a.reopen = 0;
    raw_mode();

    {
        kt_dec dec;
        kt_chord ch;
        int16_t buf[4096];
        unsigned seen = 0;
        uint8_t shown[64];
        int n_shown = -1;
        double t0 = now_s(), emu_t = 0;
        memset(&dec, 0, sizeof dec);
        memset(&ch, 0, sizeof ch);
        session_cells(a.s, shown, sizeof shown, &seen);
        while (!a.quit && !a.reopen) {
            /* the device or its rate changed: follow it */
            if (a.sound && audio_changed()) {
                unsigned was = audio_rate();
                if (audio_reopen(err, sizeof err)) { sayf("%s; running silent.", err); a.sound = 0; }
                else if (audio_rate() != was) {
                    snprintf(err, sizeof err, "Sound now at %u Hz.", audio_rate());
                    say(err);
                }
                t0 = now_s();
                emu_t = 0;
            }
            /* the unit, paced by the sound device (or the clock, silent) */
            for (;;) {
                if (a.sound) {
                    unsigned rate = audio_rate();
                    size_t n = (size_t)(rate * SLICE_S);
                    if (audio_queued() >= (size_t)(rate * AHEAD_S)) break;
                    session_render(a.s, buf, n, rate);
                    audio_write(buf, n);
                } else {
                    if (emu_t >= now_s() - t0) break;
                    session_run_silent(a.s, SLICE_S);
                    emu_t += SLICE_S;
                }
            }
            if (a.release_at > 0 && now_s() >= a.release_at) {   /* the hard reset's I-chord comes up */
                session_keys(a.s, 0, 0);
                a.release_at = 0;
            }
            if (a.braille) {
                uint8_t c[64];
                unsigned changes;
                int n = session_cells(a.s, c, sizeof c, &changes), j;
                if (changes != seen && (n != n_shown || memcmp(c, shown, (size_t)n))) {
                    char line[64 * 3 + 1], *p = line;
                    for (j = 0; j < n; j++) {
                        unsigned cp = 0x2800u + c[j];
                        *p++ = (char)(0xE0 | (cp >> 12));
                        *p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
                        *p++ = (char)(0x80 | (cp & 0x3F));
                    }
                    *p = 0;
                    say(line);
                    memcpy(shown, c, (size_t)n);
                    n_shown = n;
                }
                seen = changes;
            }
            /* keys */
            {
                fd_set rf;
                struct timeval tv = {0, 2000};
                unsigned char in[256];
                int keys[256], nk = 0, k, dots, space;
                ssize_t r = 0;
                FD_ZERO(&rf);
                FD_SET(0, &rf);
                if (select(1, &rf, NULL, NULL, &tv) > 0) {
                    r = read(0, in, sizeof in);
                    if (r == 0 && !tty) a.quit = 1;          /* the input closed */
                }
                nk = kt_decode(&dec, in, r > 0 ? (int)r : 0, keys, 256);
                for (k = 0; k < nk && !a.quit; k++) {
                    if (a.mode != M_OFF) line_key(&a, keys[k]);
                    else unit_key(&a, &ch, keys[k], now_s());
                }
                if (kt_chord_due(&ch, now_s(), &dots, &space)) chord(&a, dots, space);
            }
        }
    }

    if (a.reopen) {                        /* new firmware: the unit switched off, saved, and started on it */
        session_close(a.s);
        goto open_unit;
    }
    raw_off();
    say("Switching off and saving.");
    if (a.holding) session_keys(a.s, 0, 0);
    session_close(a.s);
    if (a.sound) audio_close();
    settings_free(a.ini);
    return 0;
}
