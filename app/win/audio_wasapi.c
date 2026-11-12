/* audio_wasapi.c -- see audio_wasapi.h. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <string.h>
#include "audio_wasapi.h"

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined here (ksmedia.h's needs a library mingw does not always carry) */
static const GUID SUBTYPE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

#define BUFFER_100NS 600000                         /* 60 ms asked of the device */

static wa_render_fn render;
static void *render_ctx;
static HANDLE thread, quit_ev, change_ev;
static volatile LONG cur_rate;
static IMMDeviceEnumerator *enumr;

/* ---- the default device changing: an IMMNotificationClient that only raises change_ev -------------------------- */
static HRESULT STDMETHODCALLTYPE nc_qi(IMMNotificationClient *t, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IMMNotificationClient)) { *o = t; return S_OK; }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE nc_ref(IMMNotificationClient *t) { (void)t; return 1; }
static HRESULT STDMETHODCALLTYPE nc_state(IMMNotificationClient *t, LPCWSTR id, DWORD s) { (void)t; (void)id; (void)s; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_added(IMMNotificationClient *t, LPCWSTR id) { (void)t; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_removed(IMMNotificationClient *t, LPCWSTR id) { (void)t; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE nc_default(IMMNotificationClient *t, EDataFlow f, ERole r, LPCWSTR id)
{
    (void)t; (void)id;
    if (f == eRender && r == eConsole) SetEvent(change_ev);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE nc_prop(IMMNotificationClient *t, LPCWSTR id, const PROPERTYKEY k)
{
    (void)t; (void)id; (void)k;
    return S_OK;
}
static IMMNotificationClientVtbl nc_vtbl = {nc_qi, nc_ref, nc_ref, nc_state, nc_added, nc_removed, nc_default, nc_prop};
static IMMNotificationClient notifier = {&nc_vtbl};

/* ---- one stream, until it ends (quit, a device change, the device gone) ------------------------------------------ */
static int play(void)                               /* 1: open it again; 0: quit */
{
    IMMDevice *dev = NULL;
    IAudioClient *ac = NULL;
    IAudioRenderClient *rc = NULL;
    WAVEFORMATEX *wf = NULL;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    UINT32 size = 0;
    int again = 1, isfloat = 0, ch, bits;
    static int16_t mono[48000];
    HRESULT hr;

    hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumr, eRender, eConsole, &dev);
    if (FAILED(hr)) goto wait;
    hr = IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&ac);
    if (FAILED(hr)) goto wait;
    hr = IAudioClient_GetMixFormat(ac, &wf);
    if (FAILED(hr)) goto wait;
    ch = wf->nChannels;
    bits = wf->wBitsPerSample;
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) isfloat = 1;
    else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        isfloat = IsEqualGUID(&((WAVEFORMATEXTENSIBLE *)wf)->SubFormat, &SUBTYPE_FLOAT);
    if (!isfloat && bits != 16) goto wait;          /* float or 16-bit: what shared mode gives */
    hr = IAudioClient_Initialize(ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, BUFFER_100NS, 0,
                                 wf, NULL);
    if (FAILED(hr)) goto wait;
    IAudioClient_SetEventHandle(ac, ev);
    IAudioClient_GetBufferSize(ac, &size);
    IAudioClient_GetService(ac, &IID_IAudioRenderClient, (void **)&rc);
    InterlockedExchange(&cur_rate, (LONG)wf->nSamplesPerSec);
    IAudioClient_Start(ac);
    for (;;) {
        HANDLE w[3] = {quit_ev, change_ev, ev};
        DWORD r = WaitForMultipleObjects(3, w, FALSE, 200);
        UINT32 pad = 0, n, i;
        BYTE *out;
        int c;
        if (r == WAIT_OBJECT_0) { again = 0; break; }
        if (r == WAIT_OBJECT_0 + 1) break;
        hr = IAudioClient_GetCurrentPadding(ac, &pad);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) break;
        n = size - pad;
        if (n > sizeof mono / sizeof *mono) n = sizeof mono / sizeof *mono;
        if (!n || FAILED(IAudioRenderClient_GetBuffer(rc, n, &out))) continue;
        render(render_ctx, mono, n, wf->nSamplesPerSec);
        for (i = 0; i < n; i++)
            for (c = 0; c < ch; c++) {
                if (isfloat) ((float *)out)[i * ch + c] = mono[i] / 32768.0f;
                else ((int16_t *)out)[i * ch + c] = mono[i];
            }
        IAudioRenderClient_ReleaseBuffer(rc, n, 0);
    }
    IAudioClient_Stop(ac);
    goto done;
wait:                                               /* no usable device now: try again in a moment */
    if (WaitForSingleObject(quit_ev, 1000) == WAIT_OBJECT_0) again = 0;
done:
    InterlockedExchange(&cur_rate, 0);
    if (rc) IAudioRenderClient_Release(rc);
    if (wf) CoTaskMemFree(wf);
    if (ac) IAudioClient_Release(ac);
    if (dev) IMMDevice_Release(dev);
    CloseHandle(ev);
    return again;
}

static DWORD WINAPI audio_thread(LPVOID p)
{
    (void)p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while (play()) {}
    CoUninitialize();
    return 0;
}

int wa_start(wa_render_fn fn, void *ctx, char *err, size_t errlen)
{
    HRESULT hr;
    render = fn;
    render_ctx = ctx;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&enumr);
    if (FAILED(hr)) { snprintf(err, errlen, "no Windows audio (0x%08lx)", (unsigned long)hr); return -1; }
    quit_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    change_ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    IMMDeviceEnumerator_RegisterEndpointNotificationCallback(enumr, &notifier);
    thread = CreateThread(NULL, 0, audio_thread, NULL, 0, NULL);
    if (!thread) { snprintf(err, errlen, "cannot start the sound thread"); return -1; }
    return 0;
}

void wa_stop(void)
{
    if (!thread) return;
    SetEvent(quit_ev);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    thread = NULL;
    IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(enumr, &notifier);
    IMMDeviceEnumerator_Release(enumr);
    enumr = NULL;
}

unsigned wa_rate(void) { return (unsigned)cur_rate; }
