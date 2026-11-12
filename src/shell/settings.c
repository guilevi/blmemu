/* settings.c -- see settings.h. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings.h"
#include "fwstore.h"
#include "../u8file.h"

#define MAX_LINES 512
#define LINE 256

struct settings {
    char *line[MAX_LINES];
    int n;
    char path[FW_PATH_MAX];
};

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

settings *settings_load(void)
{
    settings *s = (settings *)calloc(1, sizeof *s);
    char buf[LINE];
    FILE *f;
    if (!s) return NULL;
    if (fw_path(FW_INI_NAME, s->path, sizeof s->path)) s->path[0] = 0;
    f = s->path[0] ? u8_fopen(s->path, "r") : NULL;
    while (f && s->n < MAX_LINES && fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\r\n")] = 0;
        s->line[s->n++] = strdup(buf);
    }
    if (f) fclose(f);
    return s;
}

void settings_free(settings *s)
{
    int i;
    if (!s) return;
    for (i = 0; i < s->n; i++) free(s->line[i]);
    free(s);
}

/* the line holding section.key: its index, or -1; *end: where the section's lines end (to add one) */
static int find(settings *s, const char *section, const char *key, int *end)
{
    int i, in = 0;
    *end = -1;
    for (i = 0; i < s->n; i++) {
        char tmp[LINE], *t, *eq;
        snprintf(tmp, sizeof tmp, "%s", s->line[i]);
        t = trim(tmp);
        if (*t == '[') {
            char *r = strchr(t, ']');
            if (r) *r = 0;
            in = !strcmp(trim(t + 1), section);
            if (in) *end = i + 1;
            continue;
        }
        if (!in || *t == ';' || *t == '#') { if (in && *t) *end = i + 1; continue; }
        eq = strchr(t, '=');
        if (eq) {
            *eq = 0;
            if (!strcmp(trim(t), key)) return i;
            *end = i + 1;
        }
    }
    return -1;
}

const char *settings_get(settings *s, const char *section, const char *key, const char *def)
{
    static char val[LINE];
    int end, i = s ? find(s, section, key, &end) : -1;
    char *eq;
    if (i < 0) return def;
    eq = strchr(s->line[i], '=');
    snprintf(val, sizeof val, "%s", trim(eq + 1));
    return val;
}

int settings_int(settings *s, const char *section, const char *key, int def)
{
    const char *v = settings_get(s, section, key, NULL);
    return v && *v ? atoi(v) : def;
}

void settings_set(settings *s, const char *section, const char *key, const char *value)
{
    char buf[LINE];
    int end, i;
    if (!s) return;
    i = find(s, section, key, &end);
    snprintf(buf, sizeof buf, "%s = %s", key, value);
    if (i >= 0) {
        free(s->line[i]);
        s->line[i] = strdup(buf);
        return;
    }
    if (s->n + 2 >= MAX_LINES) return;
    if (end < 0) {                                  /* a new section at the end */
        char head[LINE];
        snprintf(head, sizeof head, "[%s]", section);
        if (s->n && s->line[s->n - 1][0]) s->line[s->n++] = strdup("");
        s->line[s->n++] = strdup(head);
        end = s->n;
    }
    memmove(s->line + end + 1, s->line + end, (size_t)(s->n - end) * sizeof *s->line);
    s->line[end] = strdup(buf);
    s->n++;
}

void settings_set_int(settings *s, const char *section, const char *key, int value)
{
    char v[32];
    snprintf(v, sizeof v, "%d", value);
    settings_set(s, section, key, v);
}

int settings_save(settings *s)
{
    FILE *f;
    int i;
    if (!s || !s->path[0] || !(f = u8_fopen(s->path, "w"))) return -1;
    for (i = 0; i < s->n; i++) fprintf(f, "%s\n", s->line[i]);
    return fclose(f) ? -1 : 0;
}
