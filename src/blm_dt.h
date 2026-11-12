/* blm_dt.h -- the Millennium's DoubleTalk: librc8650 (the RC8650's own firmware on an emulated M16C) behind the
 * board's port 90h.
 *
 * What it adds to librc8650:
 *   - the status register the firmware polls (SR.6 talking, SR.4 ready, SR.2 almost empty);
 *   - the bytes held until the chip would act on them (a CR or NUL; a ^X at once while it speaks): input that makes
 *     no sound costs librc8650 a second of host time, and the firmware sends a burst of commands before every line.
 *   - output at any sample rate (the audio device's: 48 kHz, 44.1 kHz, or a Bluetooth headset's 16 or 24 kHz while its
 *     microphone is in use), through librc8650's own output stage.
 * Not thread-safe: one thread drives it (the emulation's).
 */
#ifndef BLM_DT_H
#define BLM_DT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct blm_dt blm_dt;

blm_dt *blm_dt_new(const char *srec_path, char *err, int errlen);
void blm_dt_free(blm_dt *d);

void blm_dt_write(blm_dt *d, uint8_t byte);        /* a byte the firmware wrote to 90h */
uint8_t blm_dt_status(blm_dt *d);                   /* its status register */

/* the output rate: 0 the chip's own 10017 Hz, else the device's (8000-192000) */
void blm_dt_set_rate(blm_dt *d, unsigned hz);
unsigned blm_dt_rate(const blm_dt *d);
/* n samples at that rate: speech when there is some, else silence.  Always fills all n. */
void blm_dt_render(blm_dt *d, int16_t *out, size_t n);


#ifdef __cplusplus
}
#endif
#endif
