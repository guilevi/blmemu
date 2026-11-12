/* settings.h -- blmemu.ini in the data folder: [section] and key = value lines, read and written back with the
 * person's own lines (comments, settings this version does not know) kept as they were.  Portable C.
 */
#ifndef BLM_SETTINGS_H
#define BLM_SETTINGS_H

typedef struct settings settings;

settings *settings_load(void);                     /* the data folder's blmemu.ini; empty when there is none */
void settings_free(settings *s);
const char *settings_get(settings *s, const char *section, const char *key, const char *def);
int settings_int(settings *s, const char *section, const char *key, int def);
void settings_set(settings *s, const char *section, const char *key, const char *value);
void settings_set_int(settings *s, const char *section, const char *key, int value);
int settings_save(settings *s);                    /* 0, or -1 */

#endif
