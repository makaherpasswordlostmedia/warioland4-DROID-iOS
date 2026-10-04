#ifndef WL4_AUDIO_H
#define WL4_AUDIO_H
#include <stdint.h>
/* Audio output for the port.  The game thread pushes one GBA frame of mixed PCM per SoundMain() call (signed 8-bit,
 * separate right/left buffers, `rate` = GBA mixer rate e.g. 13379 Hz).  A real-time output callback (AAudio on Android)
 * resamples to the device rate with a small adaptive ratio so the game clock and the audio clock never drift apart. */
void wl4_audio_start(void);
void wl4_audio_stop(void);
void wl4_audio_pause(int paused);
void wl4_audio_poll(void);                    /* game thread, once per frame: restarts the stream after a route change */
void wl4_audio_push(const int8_t *right, const int8_t *left, int count, int rate);
#endif
