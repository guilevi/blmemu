/* audio.h -- sound out at the output device's own sample rate.
 *
 * The emulation writes 16-bit mono samples at audio_rate(); the device plays them.  The rate is the device's nominal
 * rate, read when the stream opens and again whenever the default output device changes or its rate does (a
 * Bluetooth headset drops to 16 or 24 kHz when its microphone comes on): audio_changed() then says so, and the
 * owner calls audio_reopen() and renders at the new rate from there on.  Nothing is resampled here.
 */
#ifndef BLM_AUDIO_H
#define BLM_AUDIO_H

#include <stddef.h>
#include <stdint.h>

int audio_open(char *err, int errlen);       /* 0, or -1 with a reason */
void audio_close(void);
unsigned audio_rate(void);                   /* the rate the stream runs at now */
int audio_changed(void);                     /* the device or its rate changed since the last audio_reopen */
int audio_reopen(char *err, int errlen);     /* follow it: the stream restarts at the device's rate, emptied */
size_t audio_queued(void);                   /* samples waiting to be played */
size_t audio_write(const int16_t *s, size_t n);   /* queue them; returns how many fitted */
unsigned long audio_underruns(void);         /* times the device asked and nothing was there */

#endif
