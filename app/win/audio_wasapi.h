/* audio_wasapi.h -- sound on Windows: WASAPI, shared mode, at the default device's own mix rate.  A thread of its own
 * asks the renderer for each block, at the device's pace; when the default device changes (a headset plugged in,
 * Bluetooth switching to its headset profile) or the one in use goes away, the stream is opened again on the new
 * one, at its rate. */
#ifndef BLM_AUDIO_WASAPI_H
#define BLM_AUDIO_WASAPI_H

#include <stddef.h>
#include <stdint.h>

/* n mono samples at rate Hz, from the sound thread */
typedef void (*wa_render_fn)(void *ctx, int16_t *out, size_t n, unsigned rate);

int wa_start(wa_render_fn fn, void *ctx, char *err, size_t errlen);   /* 0, or -1 with a reason */
void wa_stop(void);
unsigned wa_rate(void);                                                /* the rate in use, 0 when not running */

#endif
