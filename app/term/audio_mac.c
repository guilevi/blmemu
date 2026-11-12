/* audio_mac.c -- audio.h on Core Audio: the default output unit, fed at the default device's nominal rate. */
#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "audio.h"

#define RING (1u << 18)                      /* samples: over a second at 192 kHz */

static AudioComponentInstance unit;
static int16_t ring[RING];
static _Atomic unsigned head, tail;          /* head: written by the emulation, tail: by the device */
static _Atomic int changed;
static _Atomic unsigned long underruns;
static unsigned rate;
static AudioObjectID watched = kAudioObjectUnknown;

static OSStatus render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                       UInt32 frames, AudioBufferList *io)
{
    int16_t *out = (int16_t *)io->mBuffers[0].mData;
    unsigned t = atomic_load_explicit(&tail, memory_order_relaxed);
    unsigned h = atomic_load_explicit(&head, memory_order_acquire);
    UInt32 i;
    (void)ref; (void)flags; (void)ts; (void)bus;
    for (i = 0; i < frames; i++) {
        if (t == h) {
            memset(out + i, 0, (frames - i) * sizeof *out);
            atomic_fetch_add(&underruns, 1);
            break;
        }
        out[i] = ring[t++ % RING];
    }
    atomic_store_explicit(&tail, t, memory_order_release);
    return noErr;
}

static OSStatus on_change(AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a, void *ref)
{
    (void)obj; (void)n; (void)a; (void)ref;
    atomic_store(&changed, 1);
    return noErr;
}

static AudioObjectID default_device(void)
{
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 sz = sizeof dev;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev);
    return dev;
}

static unsigned device_rate(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    Float64 r = 0;
    UInt32 sz = sizeof r;
    if (dev == kAudioObjectUnknown || AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &r) != noErr || r < 8000)
        return 48000;
    return (unsigned)(r + 0.5);
}

static void watch_device(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    if (watched != kAudioObjectUnknown)
        AudioObjectRemovePropertyListener(watched, &a, on_change, NULL);
    watched = dev;
    if (dev != kAudioObjectUnknown)
        AudioObjectAddPropertyListener(dev, &a, on_change, NULL);
}

static int start(char *err, int errlen)
{
    AudioStreamBasicDescription f;
    AudioObjectID dev = default_device();
    OSStatus s;
    rate = device_rate(dev);
    watch_device(dev);
    memset(&f, 0, sizeof f);
    f.mSampleRate = rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 1;
    f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 2;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = 2;
    s = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (s == noErr) s = AudioUnitInitialize(unit);
    if (s == noErr) s = AudioOutputUnitStart(unit);
    if (s != noErr) {
        snprintf(err, (size_t)errlen, "Core Audio would not start at %u Hz (error %d)", rate, (int)s);
        return -1;
    }
    return 0;
}

int audio_open(char *err, int errlen)
{
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput,
                                   kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    AURenderCallbackStruct cb = {render, NULL};
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    if (!c || AudioComponentInstanceNew(c, &unit) != noErr) {
        snprintf(err, (size_t)errlen, "no default audio output");
        return -1;
    }
    AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &a, on_change, NULL);
    return start(err, errlen);
}

int audio_reopen(char *err, int errlen)
{
    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    atomic_store(&changed, 0);
    atomic_store(&tail, atomic_load(&head));   /* what was queued was rendered at the old rate */
    return start(err, errlen);
}

void audio_close(void)
{
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                    kAudioObjectPropertyElementMain};
    if (!unit) return;
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &a, on_change, NULL);
    watch_device(kAudioObjectUnknown);
    AudioOutputUnitStop(unit);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
    unit = NULL;
}

unsigned audio_rate(void) { return rate; }
int audio_changed(void) { return atomic_load(&changed); }
unsigned long audio_underruns(void) { return atomic_load(&underruns); }

size_t audio_queued(void)
{
    return atomic_load_explicit(&head, memory_order_relaxed) - atomic_load_explicit(&tail, memory_order_acquire);
}

size_t audio_write(const int16_t *s, size_t n)
{
    unsigned h = atomic_load_explicit(&head, memory_order_relaxed);
    size_t room = RING - audio_queued(), i;
    if (n > room) n = room;
    for (i = 0; i < n; i++) ring[(h + i) % RING] = s[i];
    atomic_store_explicit(&head, h + (unsigned)n, memory_order_release);
    return n;
}
