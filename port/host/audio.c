/* Audio output: lock-free SPSC ring (game thread -> audio thread) + linear resampler with drift control. */
#include "audio.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <aaudio/AAudio.h>
#endif
#ifdef __APPLE__
#include <TargetConditionals.h>
#include <AudioToolbox/AudioToolbox.h>
#endif

#define RING_FRAMES 16384u                       /* source-rate stereo frames, power of two (~1.2 s at 13379 Hz) */
#define RING_MASK   (RING_FRAMES - 1)

static int16_t g_ring[RING_FRAMES][2];           /* [0] = left, [1] = right */
static _Atomic uint32_t g_w, g_r;                /* running frame counters: producer owns g_w, consumer owns g_r */
static _Atomic int g_rate = 13379, g_paused, g_flush;
static double g_pos;                             /* consumer: fractional read position between ring[r] and ring[r+1] */
static int g_primed;                             /* consumer: false until ~40 ms are buffered (also after an underrun) */
static int16_t g_last[2];
static _Atomic int g_needRestart;                /* set by the platform error/interruption path, consumed by wl4_audio_poll() */

void wl4_audio_request_restart(void) { atomic_store(&g_needRestart, 1); }

void wl4_audio_push(const int8_t *right, const int8_t *left, int count, int rate) {
    atomic_store_explicit(&g_rate, rate > 4000 ? rate : 13379, memory_order_relaxed);
    if (atomic_load_explicit(&g_paused, memory_order_relaxed)) return;
    uint32_t w = atomic_load_explicit(&g_w, memory_order_relaxed), r = atomic_load_explicit(&g_r, memory_order_acquire);
    uint32_t used = w - r, cap = (uint32_t)atomic_load_explicit(&g_rate, memory_order_relaxed) / 8;   /* never queue more than ~125 ms: */
    if (cap > RING_FRAMES) cap = RING_FRAMES;                                                        /* a stalled consumer or an extra SoundMain() */
    uint32_t room = used < cap ? cap - used : 0;                                                     /* must not turn into latency */
    if ((uint32_t)count > room) count = (int)room;
    for (int i = 0; i < count; i++) {
        uint32_t k = (w + (uint32_t)i) & RING_MASK;
        g_ring[k][0] = (int16_t)(left[i] * 256);
        g_ring[k][1] = (int16_t)(right[i] * 256);
    }
    atomic_store_explicit(&g_w, w + (uint32_t)count, memory_order_release);
}

/* Fill `frames` interleaved stereo int16 frames at `outRate`. Runs on the audio thread. */
static void render(int16_t *out, int frames, int outRate) {
    if (atomic_load_explicit(&g_paused, memory_order_relaxed)) { memset(out, 0, (size_t)frames * 4); g_primed = 0; return; }
    uint32_t w = atomic_load_explicit(&g_w, memory_order_acquire), r = atomic_load_explicit(&g_r, memory_order_relaxed);
    if (atomic_exchange_explicit(&g_flush, 0, memory_order_relaxed)) { r = w; g_pos = 0; g_primed = 0; }
    int srcRate = atomic_load_explicit(&g_rate, memory_order_relaxed);
    double base = (double)srcRate / (double)outRate, target = srcRate * 0.040;   /* keep ~40 ms buffered */
    if (!g_primed) {
        if ((double)(w - r) >= target) g_primed = 1;
        else { memset(out, 0, (size_t)frames * 4); atomic_store_explicit(&g_r, r, memory_order_release); return; }
    }
    double err = ((double)(w - r) - target) / target;                              /* >0: too full -> consume faster */
    double adj = err * 0.02; if (adj > 0.01) adj = 0.01; if (adj < -0.01) adj = -0.01;
    double ratio = base * (1.0 + adj);
    for (int i = 0; i < frames; i++) {
        if (w - r < 2) {                                                           /* underrun: fade out, wait for refill */
            g_last[0] = (int16_t)(g_last[0] / 2); g_last[1] = (int16_t)(g_last[1] / 2);
            out[2 * i] = g_last[0]; out[2 * i + 1] = g_last[1]; g_primed = 0; continue;
        }
        const int16_t *a = g_ring[r & RING_MASK], *b = g_ring[(r + 1) & RING_MASK];
        float f = (float)g_pos;
        g_last[0] = (int16_t)(a[0] + (b[0] - a[0]) * f); g_last[1] = (int16_t)(a[1] + (b[1] - a[1]) * f);
        out[2 * i] = g_last[0]; out[2 * i + 1] = g_last[1];
        g_pos += ratio;
        while (g_pos >= 1.0) { g_pos -= 1.0; r++; if (w - r < 2) break; }
    }
    atomic_store_explicit(&g_r, r, memory_order_release);
}

void wl4_audio_pause(int paused) {
    atomic_store_explicit(&g_paused, paused, memory_order_relaxed);
    if (!paused) atomic_store_explicit(&g_flush, 1, memory_order_relaxed);        /* drop stale audio on resume */
}

#ifdef __ANDROID__
#include <android/log.h>
static AAudioStream *g_stream;

static aaudio_data_callback_result_t data_cb(AAudioStream *s, void *user, void *data, int32_t frames) {
    (void)user; render((int16_t *)data, frames, AAudioStream_getSampleRate(s));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void error_cb(AAudioStream *s, void *user, aaudio_result_t err) {        /* e.g. headphones unplugged: restart on the game thread */
    (void)s; (void)user; (void)err; atomic_store(&g_needRestart, 1);
}
static void open_stream(void) {
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setDirection(b, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 48000);
    AAudioStreamBuilder_setDataCallback(b, data_cb, NULL);
    AAudioStreamBuilder_setErrorCallback(b, error_cb, NULL);
    aaudio_result_t res = AAudioStreamBuilder_openStream(b, &g_stream);
    AAudioStreamBuilder_delete(b);
    if (res != AAUDIO_OK) { g_stream = NULL; fprintf(stderr, "wl4: audio: openStream failed: %s\n", AAudio_convertResultToText(res)); return; }
    atomic_store(&g_flush, 1);
    res = AAudioStream_requestStart(g_stream);
    fprintf(stderr, "wl4: audio: %d Hz, %d frames/burst, start=%s\n", AAudioStream_getSampleRate(g_stream),
            AAudioStream_getFramesPerBurst(g_stream), AAudio_convertResultToText(res));
}
static void close_stream(void) {
    if (!g_stream) return;
    AAudioStream_requestStop(g_stream); AAudioStream_close(g_stream); g_stream = NULL;
}
void wl4_audio_start(void) { atomic_store(&g_w, 0); atomic_store(&g_r, 0); g_pos = 0; g_primed = 0; atomic_store(&g_paused, 0); atomic_store(&g_needRestart, 0); open_stream(); }
void wl4_audio_stop(void) { close_stream(); }
/* called from wl4_audio_push's owner (game thread) once per frame */
void wl4_audio_poll(void) { if (atomic_exchange(&g_needRestart, 0)) { close_stream(); open_stream(); } }
#elif defined(__APPLE__)
/* iOS: RemoteIO output unit, signed 16-bit stereo at 48 kHz.  RemoteIO converts to the hardware rate itself, so the
 * resampler in render() only has to bridge the GBA mixer rate.  The AVAudioSession (category + interruptions) is
 * handled by the Objective-C shell, which calls wl4_audio_request_restart() when the session comes back. */
#define APPLE_OUT_RATE 48000
static AudioUnit g_au;
static int g_retry;

static OSStatus au_render(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus,
                          UInt32 frames, AudioBufferList *io) {
    (void)ref; (void)flags; (void)ts; (void)bus;
    if (io->mNumberBuffers < 1 || !io->mBuffers[0].mData) return noErr;
    render((int16_t *)io->mBuffers[0].mData, (int)frames, APPLE_OUT_RATE);
    return noErr;
}
static void open_stream(void) {
    AudioComponentDescription d = { kAudioUnitType_Output, kAudioUnitSubType_RemoteIO, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    if (!c || AudioComponentInstanceNew(c, &g_au) != noErr) { g_au = NULL; fprintf(stderr, "wl4: audio: RemoteIO unavailable\n"); return; }
    AudioStreamBasicDescription f; memset(&f, 0, sizeof f);
    f.mSampleRate = APPLE_OUT_RATE; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = 16; f.mChannelsPerFrame = 2; f.mFramesPerPacket = 1; f.mBytesPerFrame = 4; f.mBytesPerPacket = 4;
    AURenderCallbackStruct cb; cb.inputProc = au_render; cb.inputProcRefCon = NULL;
    OSStatus e1 = AudioUnitSetProperty(g_au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    OSStatus e2 = AudioUnitSetProperty(g_au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    OSStatus e3 = AudioUnitInitialize(g_au);
    OSStatus e4 = (e1 == noErr && e2 == noErr && e3 == noErr) ? AudioOutputUnitStart(g_au) : -1;
    atomic_store(&g_flush, 1);
    fprintf(stderr, "wl4: audio: RemoteIO %d Hz, format=%d callback=%d init=%d start=%d\n", APPLE_OUT_RATE, (int)e1, (int)e2, (int)e3, (int)e4);
    if (e4 != noErr) { AudioComponentInstanceDispose(g_au); g_au = NULL; }
}
static void close_stream(void) {
    if (!g_au) return;
    AudioOutputUnitStop(g_au); AudioUnitUninitialize(g_au); AudioComponentInstanceDispose(g_au); g_au = NULL;
}
void wl4_audio_start(void) { atomic_store(&g_w, 0); atomic_store(&g_r, 0); g_pos = 0; g_primed = 0; atomic_store(&g_paused, 0); atomic_store(&g_needRestart, 0); g_retry = 0; open_stream(); }
void wl4_audio_stop(void) { close_stream(); }
void wl4_audio_poll(void) {
    if (atomic_exchange(&g_needRestart, 0)) { close_stream(); open_stream(); }
    else if (!g_au && ++g_retry % 120 == 0) open_stream();       /* session was not active yet: retry about every 2 s */
}
#else
void wl4_audio_start(void) {}
void wl4_audio_stop(void) {}
void wl4_audio_poll(void) {}
#endif
