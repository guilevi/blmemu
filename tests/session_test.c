/* Private-firmware acceptance: session_test BNS RC8650_SREC BASELINE_STATE.
 * BLMEMU_HOME must name a disposable directory. This executable never opens audio or a window. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "session.h"
#include "fwstore.h"

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

static int ready, heard_test;
static unsigned long samples;
#ifdef TEST_ALLOC_FAILURE
static int fail_calloc;
void *__real_calloc(size_t n, size_t size);
void *__wrap_calloc(size_t n, size_t size)
{
    if (fail_calloc) { fail_calloc = 0; return NULL; }
    return __real_calloc(n, size);
}
#endif
static void note(void *ctx, const char *text) { (void)ctx; fprintf(stderr, "%s\n", text); }
static void speech(void *ctx, const char *text)
{
    (void)ctx;
    if (strstr(text, "READY")) ready++;
    if (strstr(text, "TEST")) heard_test++;
}
static void render(blm_session *s, unsigned seconds)
{
    int16_t audio[256];
    size_t remaining = seconds * 10017u;
    while (remaining) {
        size_t n = remaining > 256 ? 256 : remaining, i;
        session_render(s, audio, n, 10017);
        for (i = 0; i < n; i++) samples += audio[i] != 0;
        remaining -= n;
    }
}
int main(int argc, char **argv)
{
    char what[256], err[512], path[FW_PATH_MAX];
    uint8_t *state, cells[64];
    long n;
    FILE *f;
    blm_session *s;
    int16_t audio[256];
    unsigned changes;
    CHECK(argc == 4 && getenv("BLMEMU_HOME") != NULL);
    CHECK(fw_import_bns(argv[1], what, sizeof what, err, sizeof err) == 0);
    CHECK(fw_import_rc8650(argv[2], what, sizeof what, err, sizeof err) == 0);
    state = fw_read_all(argv[3], &n); CHECK(state != NULL);
    CHECK(fw_path(FW_STATE_NAME, path, sizeof path) == 0);
    f = fopen(path, "wb"); CHECK(f != NULL);
    CHECK(fwrite(state, 1, (size_t)n, f) == (size_t)n); CHECK(fclose(f) == 0); free(state);
    s = session_open(note, NULL, err, sizeof err); CHECK(s != NULL);
    session_on_speech(s, speech, NULL); render(s, 5);
    CHECK(ready == 1);
    session_chord(s, 1, 1); render(s, 1);
    session_chord(s, 0x3b, 1); render(s, 3);
    CHECK(heard_test > 0 && samples > 0);
    CHECK(session_cells(s, cells, sizeof cells, &changes) == 40 && changes > 0);
    session_restart(s, 0, 0); render(s, 5); CHECK(ready == 2);
    session_keys(s, 3, 0); render(s, 1); session_keys(s, 0, 0); render(s, 1);
    CHECK(session_save(s, err, sizeof err) == 0);
#ifdef TEST_ALLOC_FAILURE
    fail_calloc = 1;
    CHECK(session_factory(s, err, sizeof err) == -1);
    CHECK(strstr(err, "out of memory") != NULL);
    CHECK(session_cells(s, cells, sizeof cells, &changes) == 0 && changes == 0);
    session_chord(s, 1, 0); session_keys(s, 0, 0); session_display_mode(s, 0);
    memset(audio, 0x7f, sizeof audio); session_render(s, audio, 256, 10017);
    for (n = 0; n < 256; n++) CHECK(audio[n] == 0);
    CHECK(session_save(s, err, sizeof err) == -1);
#else
    (void)audio;
#endif
    session_close(s);
    puts("session firmware tests passed");
    return 0;
}
