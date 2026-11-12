/* audio_alsa.c -- audio.h on ALSA (Linux): the "default" device, which on a desktop is PulseAudio or PipeWire and
 * takes any rate.  48 kHz is asked for; the rate the device grants is the one rendered at.  The device itself is the
 * queue: audio_queued is its delay. */
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"

static snd_pcm_t *pcm;
static unsigned rate;
static unsigned long underruns;

static int start(char *err, int errlen)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_uframes_t buffer = 0, period = 0;
    int r;
    unsigned want = 48000;
    const char *dev = getenv("BLMEMU_ALSA_DEVICE");
    if ((r = snd_pcm_open(&pcm, dev && *dev ? dev : "default", SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK)) < 0) {
        snprintf(err, (size_t)errlen, "no sound device (%s)", snd_strerror(r));
        return -1;
    }
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16);
    snd_pcm_hw_params_set_channels(pcm, hw, 1);
    snd_pcm_hw_params_set_rate_near(pcm, hw, &want, NULL);
    buffer = want / 5;                                    /* 200 ms of room; the program keeps ~60 ms in it */
    period = want / 100;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, NULL);
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buffer);
    if ((r = snd_pcm_hw_params(pcm, hw)) < 0) {
        snprintf(err, (size_t)errlen, "the sound device refused 16-bit mono (%s)", snd_strerror(r));
        snd_pcm_close(pcm);
        pcm = NULL;
        return -1;
    }
    rate = want;
    snd_pcm_prepare(pcm);
    return 0;
}

int audio_open(char *err, int errlen) { return start(err, errlen); }

int audio_reopen(char *err, int errlen)
{
    if (pcm) snd_pcm_close(pcm);
    pcm = NULL;
    return start(err, errlen);
}

void audio_close(void)
{
    if (pcm) {
        snd_pcm_drain(pcm);
        snd_pcm_close(pcm);
    }
    pcm = NULL;
}

unsigned audio_rate(void) { return rate; }
int audio_changed(void) { return 0; }                     /* the sound server converts when the device changes */
unsigned long audio_underruns(void) { return underruns; }

size_t audio_queued(void)
{
    snd_pcm_sframes_t d = 0;
    if (!pcm) return 0;
    if (snd_pcm_delay(pcm, &d) < 0) {
        underruns++;
        snd_pcm_recover(pcm, -EPIPE, 1);
        return 0;
    }
    return d > 0 ? (size_t)d : 0;
}

size_t audio_write(const int16_t *s, size_t n)
{
    size_t done = 0;
    while (pcm && done < n) {
        snd_pcm_sframes_t w = snd_pcm_writei(pcm, s + done, n - done);
        if (w == -EAGAIN) break;                          /* full: the rest is dropped, the queue is long enough */
        if (w < 0) {
            underruns++;
            if (snd_pcm_recover(pcm, (int)w, 1) < 0) break;
            continue;
        }
        done += (size_t)w;
    }
    return done;
}
