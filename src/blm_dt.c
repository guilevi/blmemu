/* blm_dt.c -- see blm_dt.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "blm_dt.h"
#include "rc8650.h"
#include "u8file.h"

struct blm_dt {
    rc8650_t *e;
    unsigned rate;
    /* Bytes held until the chip would act on them: it executes nothing before a CR or NUL (datasheet), and
       librc8650 spends about a second of host time on input that makes no sound (it waits out its quiet period),
       which the firmware's command bursts (^X ^A0O ^A5V ... before every line) would otherwise cost each time. */
    unsigned char held[4096];
    int n_held;
};

blm_dt *blm_dt_new(const char *srec, char *err, int errlen)
{
    blm_dt *d = (blm_dt *)calloc(1, sizeof *d);
    char *e = NULL;
    if (!d) { snprintf(err, (size_t)errlen, "out of memory"); return NULL; }
    {   /* read here (a UTF-8 path on every system), handed over in memory */
        FILE *f = u8_fopen(srec, "rb");
        long n = 0;
        char *buf = NULL;
        if (f && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0 &&
            (buf = (char *)malloc((size_t)n)) != NULL && fread(buf, 1, (size_t)n, f) == (size_t)n)
            d->e = rc8650_new_from_memory(buf, (size_t)n, &e);
        else
            e = NULL;
        if (f) fclose(f);
        free(buf);
    }
    if (!d->e) {
        snprintf(err, (size_t)errlen, "%s: %s", srec, e ? e : "cannot start the RC8650");
        free(e);
        free(d);
        return NULL;
    }
    return d;
}

void blm_dt_free(blm_dt *d)
{
    if (!d) return;
    rc8650_free(d->e);
    free(d);
}

static void pass(blm_dt *d, const unsigned char *p, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char b = p[i];
        if (b == 0x18) {                     /* ^X: stop.  Speaking: at once.  Idle: it only discards what is held */
            if (rc8650_is_speaking(d->e)) {
                if (d->n_held) rc8650_write(d->e, d->held, (size_t)d->n_held);
                rc8650_write(d->e, &b, 1);
            }
            d->n_held = 0;
            continue;
        }
        if (d->n_held < (int)sizeof d->held) d->held[d->n_held++] = b;
        if (b == 0x0D || b == 0x00 || d->n_held == (int)sizeof d->held) {
            rc8650_write(d->e, d->held, (size_t)d->n_held);
            d->n_held = 0;
        }
    }
}

void blm_dt_write(blm_dt *d, uint8_t b)
{
    pass(d, &b, 1);
}

uint8_t blm_dt_status(blm_dt *d)
{
    uint8_t s = 0x10;                        /* ready: librc8650's input queue grows, it never fills */
    if (rc8650_is_speaking(d->e)) s |= 0x40;
    else s |= 0x04;
    return s;
}

void blm_dt_set_rate(blm_dt *d, unsigned hz)
{
    d->rate = hz == RC8650_SAMPLE_RATE ? 0 : hz;
    rc8650_set_output_rate(d->e, d->rate);
}

unsigned blm_dt_rate(const blm_dt *d) { return d->rate ? d->rate : RC8650_SAMPLE_RATE; }

void blm_dt_render(blm_dt *d, int16_t *out, size_t n)
{
    size_t got = 0;
    while (got < n && rc8650_is_speaking(d->e)) {
        size_t g = rc8650_read_s16(d->e, out + got, n - got, NULL);
        if (!g) break;
        got += g;
    }
    if (got < n) memset(out + got, 0, (n - got) * sizeof *out);
}

