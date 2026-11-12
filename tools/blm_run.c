/* blm_run.c -- run the Braille Lite Millennium headless: boot, press keys on a schedule, print what it sends the
 * DoubleTalk as text, and (with --wav) render the speech through librc8650.
 *
 *   blm_run BLM40ENU.BNS [--factory] [--state F] [--save F] [--seconds S] [--wav out.wav] [--dt-fw rc8650.srec]
 *           [--key T:CHORD ...] [--yes N] [--trace io,far,dt,keys,unknown]
 *
 * CHORD: braille dots as digits (1-8), s for the space bar: "s24" is I-chord, "13456" is y.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/blm_board.h"
#include "../src/blm_dt.h"
#define RC8650_SAMPLE_RATE 10017

#define SLICE_S 0.005

typedef struct {
    blm_dt *dt;
    unsigned char text[8192];
    int n;
    int yes_left;
    double yes_at;
    double now;
    int in_cmd;                 /* inside a ^A command: digits then a letter */
} host_ctx;

static void flush_text(host_ctx *h)
{
    if (h->n) {
        h->text[h->n] = 0;
        printf("[%7.2f] SAY \"%s\"\n", h->now, h->text);
        fflush(stdout);
        h->n = 0;
    }
}

static void dt_write(void *ctx, uint8_t b)
{
    host_ctx *h = (host_ctx *)ctx;
    if (h->dt) blm_dt_write(h->dt, b);
    if (b == 0x01) { h->in_cmd = 1; return; }
    if (h->in_cmd) {
        if ((b >= '0' && b <= '9') || b == '-' || b == '+') return;
        h->in_cmd = 0;
        return;
    }
    if (b == 0x0D || b == 0x00 || b == 0x18) {
        if (h->n && h->yes_left > 0 && memchr(h->text, '?', (size_t)h->n)) {
            h->yes_left--;
            h->yes_at = h->now + 1.0;
        }
        flush_text(h);
        return;
    }
    if (b >= 32 && b < 127 && h->n < (int)sizeof h->text - 1)
        h->text[h->n++] = b;
}

static uint8_t dt_status(void *ctx)
{
    host_ctx *h = (host_ctx *)ctx;
    return h->dt ? blm_dt_status(h->dt) : 0x14;   /* ready, almost empty */
}

static int parse_chord(const char *s, int *dots, int *space)
{
    *dots = 0;
    *space = 0;
    for (; *s; s++) {
        if (*s >= '1' && *s <= '8') *dots |= 1 << (*s - '1');
        else if (*s == 's') *space = 1;
        else return -1;
    }
    return 0;
}

static void wav_header(FILE *f, unsigned rate, unsigned long n)
{
    unsigned long bytes = n * 2;
    unsigned char h[44];
    memcpy(h, "RIFF", 4);
    h[4] = (unsigned char)(bytes + 36); h[5] = (unsigned char)((bytes + 36) >> 8);
    h[6] = (unsigned char)((bytes + 36) >> 16); h[7] = (unsigned char)((bytes + 36) >> 24);
    memcpy(h + 8, "WAVEfmt ", 8);
    h[16] = 16; h[17] = h[18] = h[19] = 0; h[20] = 1; h[21] = 0; h[22] = 1; h[23] = 0;
    h[24] = (unsigned char)rate; h[25] = (unsigned char)(rate >> 8); h[26] = (unsigned char)(rate >> 16); h[27] = 0;
    h[28] = (unsigned char)(rate * 2); h[29] = (unsigned char)((rate * 2) >> 8);
    h[30] = (unsigned char)((rate * 2) >> 16); h[31] = 0;
    h[32] = 2; h[33] = 0; h[34] = 16; h[35] = 0;
    memcpy(h + 36, "data", 4);
    h[40] = (unsigned char)bytes; h[41] = (unsigned char)(bytes >> 8);
    h[42] = (unsigned char)(bytes >> 16); h[43] = (unsigned char)(bytes >> 24);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

int main(int argc, char **argv)
{
    const char *fw = NULL, *state = NULL, *save = NULL, *wav = NULL, *dtfw = NULL;
    double seconds = 30, key_t[256];
    int key_d[256], key_s[256], n_keys = 0, factory = 0, i, trace = 0;
    host_ctx h;
    memset(&h, 0, sizeof h);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--factory")) factory = 1;
        else if (!strcmp(argv[i], "--state") && i + 1 < argc) state = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) save = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
        else if (!strcmp(argv[i], "--dt-fw") && i + 1 < argc) dtfw = argv[++i];
        else if (!strcmp(argv[i], "--yes") && i + 1 < argc) h.yes_left = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) {
            const char *t = argv[++i];
            if (strstr(t, "io")) trace |= BLM_TR_IO;
            if (strstr(t, "far")) trace |= BLM_TR_FAR;
            if (strstr(t, "dt")) trace |= BLM_TR_DT;
            if (strstr(t, "keys")) trace |= BLM_TR_KEYS;
            if (strstr(t, "unknown")) trace |= BLM_TR_UNKNOWN;
            if (strstr(t, "internal")) trace |= BLM_TR_INTERNAL;
            if (strstr(t, "mem")) trace |= BLM_TR_MEM;
            if (strstr(t, "flash")) trace |= BLM_TR_FLASH;
        } else if (!strcmp(argv[i], "--key") && i + 1 < argc && n_keys < 256) {
            char *c = strchr(argv[++i], ':');
            unsigned kk, kv;
            if (c && sscanf(c + 1, "raw%x,%x", &kk, &kv) == 2) { key_d[n_keys] = (int)kv; key_s[n_keys] = 0x100 | (int)kk; }
            else if (!c || parse_chord(c + 1, &key_d[n_keys], &key_s[n_keys])) { fprintf(stderr, "bad --key\n"); return 2; }
            key_t[n_keys++] = atof(argv[i]);
        } else if (!fw) fw = argv[i];
        else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 2; }
    }
    if (!fw) {
        fprintf(stderr, "usage: blm_run BLM40ENU.BNS [options]\n"
                        "  --factory            a new unit: the hard reset (I-chord held at power-on)\n"
                        "  --yes N              answer y to the next N questions it asks\n"
                        "  --state F, --save F  start from a saved unit; save it (switched off) at the end\n"
                        "  --seconds S          how long to run (30)\n"
                        "  --key T:CHORD        a chord at T seconds: dots as digits, s the space bar (s24 = I-chord);\n"
                        "                       T:rawKK,VV sends the keyboard controller's pair as it is\n"
                        "  --wav F --dt-fw S    the speech into a WAV, with the RC8650 image S\n"
                        "  --trace LIST         io, internal, far, dt, keys, mem, flash, unknown (to stderr)\n");
        return 2;
    }

    FILE *f = fopen(fw, "rb");
    if (!f) { perror(fw); return 1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = (uint8_t *)malloc((size_t)n);
    if (fread(data, 1, (size_t)n, f) != (size_t)n) { perror(fw); return 1; }
    fclose(f);
    long off = blm_find_image(data, n);
    if (off < 0) { fprintf(stderr, "%s: no program image found\n", fw); return 1; }

    if (wav) {
        char err[256];
        if (!dtfw) dtfw = getenv("RC8650_FIRMWARE");
        if (!dtfw) { fprintf(stderr, "--wav needs --dt-fw (the RC8650's firmware .srec)\n"); return 2; }
        h.dt = blm_dt_new(dtfw, err, sizeof err);
        if (!h.dt) { fprintf(stderr, "%s\n", err); return 1; }
    }

    blm_host host = { &h, dt_write, dt_status };
    blm_unit *u = blm_create(data + off, n - off, &host);
    if (!u) { fprintf(stderr, "blm_create failed\n"); return 1; }
    if (trace) blm_trace(u, stderr, trace);
    if (state && blm_load(u, state)) { fprintf(stderr, "%s: not a state\n", state); return 1; }
    if (factory) blm_factory(u);

    FILE *wf = NULL;
    unsigned long n_samples = 0;
    double owed = 0;
    if (wav) {
        wf = fopen(wav, "wb");
        if (!wf) { perror(wav); return 1; }
        wav_header(wf, RC8650_SAMPLE_RATE, 0);
    }

    double hz = blm_clock_hz(u);
    int last_latch = 0;
    int k = 0, released_factory = 0;
    for (h.now = 0; h.now < seconds; h.now += SLICE_S) {
        if (factory && !released_factory && h.now >= 1.0) {   /* the I-chord let go once the start has read it */
            blm_keys(u, 0, 0);
            released_factory = 1;
        }
        while (k < n_keys && key_t[k] <= h.now) {
            printf("[%7.2f] KEY %s%s\n", h.now, key_s[k] ? "space+" : "", "");
            if (getenv("BLM_FETCHSTATS")) { FILE *nul = fopen("/dev/null", "w"); blm_fetch_stats(u, nul); fclose(nul); }
            if (key_s[k] & 0x100) blm_key_raw(u, key_s[k] & 0xFF, key_d[k]);
            else blm_chord(u, key_d[k], key_s[k]);
            k++;
        }
        if (h.yes_at > 0 && h.now >= h.yes_at) {
            printf("[%7.2f] KEY y\n", h.now);
            blm_chord(u, 0x3D, 0);
            h.yes_at = 0;
        }
        blm_run(u, (uint64_t)(SLICE_S * hz));
        if (blm_display_latches(u) != last_latch) {
            uint8_t c[BLM_MAX_CELLS];
            int nc = blm_cells(u, c, BLM_MAX_CELLS), j;
            static uint8_t prev[BLM_MAX_CELLS];
            static int prev_n;
            last_latch = blm_display_latches(u);
            if (nc != prev_n || memcmp(c, prev, (size_t)nc)) {
                printf("[%7.2f] CELLS ", h.now);
                for (j = 0; j < nc; j++) {           /* Unicode braille, U+2800 + dots */
                    unsigned cp = 0x2800u + c[j];
                    printf("%c%c%c", 0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F));
                }
                printf("\n");
                memcpy(prev, c, (size_t)nc);
                prev_n = nc;
            }
        }
        if (wf) {
            int16_t buf[512];
            owed += SLICE_S * RC8650_SAMPLE_RATE;
            while (owed >= 1) {
                size_t want = owed > 512 ? 512 : (size_t)owed;
                blm_dt_render(h.dt, buf, want);
                fwrite(buf, 2, want, wf);
                n_samples += want;
                owed -= (double)want;
            }
        }
    }
    flush_text(&h);
    if (getenv("BLM_FETCHSTATS")) blm_fetch_stats(u, stdout);
    printf("[%7.2f] end, pc %04X (%05X)\n", h.now, blm_pc(u), blm_phys_pc(u));
    if (getenv("BLM_MEMSTATS")) blm_mem_stats(u, stdout);
    if (getenv("BLM_DUMP")) {                 /* BLM_DUMP=physical,length: the bytes at the end, for the disassembler */
        unsigned a = 0, l = 0, j;
        sscanf(getenv("BLM_DUMP"), "%x,%x", &a, &l);
        FILE *df = fopen("blm_dump.bin", "wb");
        for (j = 0; j < l && df; j++) fputc(blm_peek(u, a + j), df);
        if (df) fclose(df);
    }
    if (wf) { wav_header(wf, RC8650_SAMPLE_RATE, n_samples); fclose(wf); }
    if (save) {                               /* switched off first, as a person would, then saved */
        int i;
        blm_power(u, 0);
        for (i = 0; i < 400 && !blm_powered_down(u); i++) blm_run(u, (uint64_t)(0.005 * hz));
        printf("[%7.2f] %s\n", h.now + i * 0.005, blm_powered_down(u) ? "switched off" : "did not power down");
    }
    if (save && blm_save(u, save)) { perror(save); return 1; }
    blm_destroy(u);
    if (h.dt) blm_dt_free(h.dt);
    return 0;
}
