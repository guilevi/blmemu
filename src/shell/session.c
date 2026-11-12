/* session.c -- see session.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session.h"
#include "fwstore.h"
#include "../blm_board.h"
#include "../blm_dt.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef CRITICAL_SECTION lock_t;
#define LOCK_INIT(l) InitializeCriticalSection(l)
#define LOCK(l) EnterCriticalSection(l)
#define UNLOCK(l) LeaveCriticalSection(l)
#define LOCK_FREE(l) DeleteCriticalSection(l)
#else
#include <pthread.h>
typedef pthread_mutex_t lock_t;
#define LOCK_INIT(l) pthread_mutex_init(l, NULL)
#define LOCK(l) pthread_mutex_lock(l)
#define UNLOCK(l) pthread_mutex_unlock(l)
#define LOCK_FREE(l) pthread_mutex_destroy(l)
#endif

#define SLICE_S 0.005
#define SAVE_EVERY_S 60.0

struct blm_session {
    lock_t lock;
    blm_unit *u;
    blm_dt *dt;
    uint8_t *data;                    /* the update file, whose image the unit runs */
    long n, off;
    char bns[FW_PATH_MAX], rc[FW_PATH_MAX], state[FW_PATH_MAX];
    session_note_fn note;
    void *note_ctx;
    void (*speech)(void *, const char *);
    void *speech_ctx;
    /* the text going to the DoubleTalk, a line at a time */
    char line[512];
    int n_line, in_cmd;
    int questions, ready;             /* the setup's: questions asked, "ready" said */
    double cyc_owed, since_save;
    int display_mode;
};

static void note(blm_session *s, const char *t)
{
    if (s->note) s->note(s->note_ctx, t);
}

/* ---- the DoubleTalk's port ----------------------------------------------------------------------------------------- */
static void dt_write(void *ctx, uint8_t b)
{
    blm_session *s = (blm_session *)ctx;
    blm_dt_write(s->dt, b);
    if (b == 0x01) { s->in_cmd = 1; return; }
    if (s->in_cmd) {                  /* ^A, digits, a letter */
        if ((b >= '0' && b <= '9') || b == '-' || b == '+') return;
        s->in_cmd = 0;
        return;
    }
    if (b == 0x0D || b == 0x00) {
        s->line[s->n_line] = 0;
        if (s->n_line) {
            if (strchr(s->line, '?')) s->questions++;
            if (strstr(s->line, "READY")) s->ready = 1;
            if (s->speech) s->speech(s->speech_ctx, s->line);
        }
        s->n_line = 0;
        return;
    }
    if (b >= 32 && b < 127 && s->n_line < (int)sizeof s->line - 1) s->line[s->n_line++] = (char)b;
}

static uint8_t dt_status(void *ctx)
{
    return blm_dt_status(((blm_session *)ctx)->dt);
}

/* ---- running ------------------------------------------------------------------------------------------------------- */
static void run_unheard(blm_session *s, double seconds)
{
    int16_t junk[256];
    double t;
    for (t = 0; t < seconds; t += SLICE_S) {
        size_t n = (size_t)(SLICE_S * blm_dt_rate(s->dt));
        blm_run(s->u, (uint64_t)(SLICE_S * blm_clock_hz(s->u)));
        /* the chip's time goes on with the unit's: the firmware waits on its talking flag */
        blm_dt_render(s->dt, junk, n > 256 ? 256 : n);
    }
}

static void switch_off(blm_session *s)
{
    int i;
    blm_power(s->u, 0);
    for (i = 0; i < 400 && !blm_powered_down(s->u); i++) run_unheard(s, SLICE_S);
}

static int save_locked(blm_session *s, char *err, size_t errlen)
{
    if (blm_save(s->u, s->state)) {
        snprintf(err, errlen, "could not save the unit's memory to %s", s->state);
        return -1;
    }
    s->since_save = 0;
    return 0;
}

/* a new unit, its memory empty, set up as from the factory: I-chord held through the start, every question y */
static int setup_locked(blm_session *s, char *err, size_t errlen)
{
    blm_host host = {s, dt_write, dt_status};
    double t, answer_at = 0;
    int asked = 0;
    if (s->u) blm_destroy(s->u);
    s->u = blm_create(s->data + s->off, s->n - s->off, &host);
    if (!s->u) { snprintf(err, errlen, "out of memory"); return -1; }
    blm_display_mode_switch(s->u, s->display_mode);
    s->questions = s->ready = 0;
    note(s, "Setting up a new unit: the hard reset, every question answered yes (a few seconds).");
    blm_factory(s->u);
    for (t = 0; t < 120 && !s->ready; t += SLICE_S) {
        if (t >= 1.0 && t < 1.0 + SLICE_S) blm_keys(s->u, 0, 0);    /* the start has read the I-chord: let it go */
        if (s->questions > asked) {                                  /* y, a second later, as a person answers */
            asked = s->questions;
            answer_at = t + 1.0;
        }
        if (answer_at > 0 && t >= answer_at) {
            answer_at = 0;
            blm_chord(s->u, 0x3D, 0);
        }
        run_unheard(s, SLICE_S);
    }
    if (!s->ready) { snprintf(err, errlen, "the new unit's setup did not finish"); return -1; }
    switch_off(s);
    if (save_locked(s, err, errlen)) return -1;
    blm_power(s->u, 1);               /* on again: the first start a person hears */
    return 0;
}

blm_session *session_open(session_note_fn fn, void *ctx, char *err, size_t errlen)
{
    blm_session *s = (blm_session *)calloc(1, sizeof *s);
    char why[256];
    blm_host host;
    if (!s) { snprintf(err, errlen, "out of memory"); return NULL; }
    LOCK_INIT(&s->lock);
    s->note = fn;
    s->note_ctx = ctx;
    if (fw_path(FW_BNS_NAME, s->bns, sizeof s->bns) || fw_path(FW_RC_NAME, s->rc, sizeof s->rc) ||
        fw_path(FW_STATE_NAME, s->state, sizeof s->state)) {
        snprintf(err, errlen, "cannot make the data folder");
        goto fail;
    }
    s->data = fw_read_all(s->bns, &s->n);
    if (!s->data) { snprintf(err, errlen, "the Millennium's firmware has not been brought in yet"); goto fail; }
    {
        fw_bns_info info;
        if (fw_check_bns(s->data, s->n, &info, why, sizeof why)) { snprintf(err, errlen, "%s: %s", s->bns, why); goto fail; }
        s->off = info.image;
    }
    s->dt = blm_dt_new(s->rc, why, sizeof why);
    if (!s->dt) { snprintf(err, errlen, "the DoubleTalk's firmware: %s", why); goto fail; }
    host.ctx = s;
    host.dt_write = dt_write;
    host.dt_status = dt_status;
    s->u = blm_create(s->data + s->off, s->n - s->off, &host);
    if (!s->u) { snprintf(err, errlen, "out of memory"); goto fail; }
    if (blm_load(s->u, s->state) && setup_locked(s, err, errlen)) goto fail;
    return s;
fail:
    if (s->u) blm_destroy(s->u);
    if (s->dt) blm_dt_free(s->dt);
    free(s->data);
    LOCK_FREE(&s->lock);
    free(s);
    return NULL;
}

void session_close(blm_session *s)
{
    char err[256];
    if (!s) return;
    LOCK(&s->lock);
    blm_keys(s->u, 0, 0);
    switch_off(s);
    if (save_locked(s, err, sizeof err)) note(s, err);
    UNLOCK(&s->lock);
    blm_destroy(s->u);
    blm_dt_free(s->dt);
    free(s->data);
    LOCK_FREE(&s->lock);
    free(s);
}

void session_render(blm_session *s, int16_t *out, size_t n, unsigned rate)
{
    char err[256];
    LOCK(&s->lock);
    if (rate && blm_dt_rate(s->dt) != rate) blm_dt_set_rate(s->dt, rate);
    s->cyc_owed += (double)n / blm_dt_rate(s->dt) * blm_clock_hz(s->u);
    blm_run(s->u, (uint64_t)s->cyc_owed);
    s->cyc_owed -= (double)(uint64_t)s->cyc_owed;
    blm_dt_render(s->dt, out, n);
    s->since_save += (double)n / blm_dt_rate(s->dt);
    if (s->since_save >= SAVE_EVERY_S && save_locked(s, err, sizeof err)) note(s, err);
    UNLOCK(&s->lock);
}

void session_run_silent(blm_session *s, double seconds)
{
    char err[256];
    LOCK(&s->lock);
    run_unheard(s, seconds);
    s->since_save += seconds;
    if (s->since_save >= SAVE_EVERY_S && save_locked(s, err, sizeof err)) note(s, err);
    UNLOCK(&s->lock);
}

void session_chord(blm_session *s, int dots, int space)
{
    LOCK(&s->lock);
    blm_chord(s->u, dots, space);
    UNLOCK(&s->lock);
}

void session_keys(blm_session *s, int dots, int space)
{
    LOCK(&s->lock);
    blm_keys(s->u, dots, space);
    UNLOCK(&s->lock);
}

void session_restart(blm_session *s, int hold_dots, int hold_space)
{
    LOCK(&s->lock);
    blm_keys(s->u, 0, 0);
    switch_off(s);
    blm_power(s->u, 1);
    if (hold_dots || hold_space) blm_keys(s->u, hold_dots, hold_space);
    UNLOCK(&s->lock);
}

int session_factory(blm_session *s, char *err, size_t errlen)
{
    int r;
    LOCK(&s->lock);
    r = setup_locked(s, err, errlen);
    UNLOCK(&s->lock);
    return r;
}

void session_display_mode(blm_session *s, int on)
{
    LOCK(&s->lock);
    s->display_mode = on != 0;
    blm_display_mode_switch(s->u, s->display_mode);
    UNLOCK(&s->lock);
}

int session_display_mode_on(blm_session *s) { return s->display_mode; }

int session_cells(blm_session *s, uint8_t *cells, int cap, unsigned *changes)
{
    int n;
    LOCK(&s->lock);
    n = blm_cells(s->u, cells, cap);
    if (changes) *changes = (unsigned)blm_display_latches(s->u);
    UNLOCK(&s->lock);
    return n;
}

void session_on_speech(blm_session *s, void (*fn)(void *, const char *), void *ctx)
{
    LOCK(&s->lock);
    s->speech = fn;
    s->speech_ctx = ctx;
    UNLOCK(&s->lock);
}

int session_save(blm_session *s, char *err, size_t errlen)
{
    int r;
    LOCK(&s->lock);
    r = save_locked(s, err, errlen);
    UNLOCK(&s->lock);
    return r;
}
