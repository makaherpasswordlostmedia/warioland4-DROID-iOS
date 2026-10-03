/* Host glue: runs the wasm2c-converted game on a worker thread and exposes a tiny
 * C API for the Android JNI layer (or any other front end). */
#include "wl4.h"
#include "../hal/hal.h"
#include "host.h"
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
/* stderr is piped into logcat + the log file by jni.c (JNI_OnLoad), so plain fprintf is enough on every platform */
#define WL4_LOG(...) (fprintf(stderr, "wl4: " __VA_ARGS__), fputc('\n', stderr))

struct w2c_env { w2c_wl4 *inst; };

static w2c_wl4 g_inst;
static struct w2c_env g_env;
static pthread_t g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_front[GBA_W * GBA_H];
static volatile int g_stop, g_running, g_frames;
static uint8_t *g_rom; static size_t g_romSize;
static int g_paused;
static uint8_t g_save[GBA_SRAM_SIZE]; static int g_haveSave;

/* A wasm trap (OOB, bad indirect call...) means a port bug: report it and stop the game thread
 * instead of crashing the app.  Build the wasm2c C files with -DWASM_RT_TRAP_HANDLER=wl4_trap. */
void wl4_trap(wasm_rt_trap_t code) {
    WL4_LOG("wasm trap %d (game thread stopped)", (int)code);
    g_running = 0; pthread_exit(NULL);
}

/* ---------- imports the wasm module expects ---------- */
void w2c_env_hal_dma_set(struct w2c_env *e, u32 ch, u32 src, u32 dst, u32 ctl) { (void)e; hal_dma_set((int)ch, src, dst, ctl); }
void w2c_env_LZ77UnCompVram(struct w2c_env *e, u32 src, u32 dst) { (void)e; bios_LZ77UnComp(src, dst); }
static char g_siteSeen[8192];
void w2c_env_hal_unported_asm(struct w2c_env *e, u32 site) {
    (void)e; if (site < sizeof g_siteSeen && !g_siteSeen[site]) { g_siteSeen[site] = 1; WL4_LOG("UNPORTED inline asm site #%u executed (see ASM_SITES.txt)", site); }
}
u32 w2c_env_hal_poll_vcount(struct w2c_env *e) { (void)e; return hal_poll_vcount(); }
void w2c_env_CPUSet(struct w2c_env *e, u32 src, u32 dst, u32 ctl) { (void)e; bios_CpuSet(src, dst, ctl); }
void w2c_env_CpuFastSet(struct w2c_env *e, u32 src, u32 dst, u32 ctl) { (void)e; bios_CpuFastSet(src, dst, ctl); }
void w2c_env_LZ77UnCompWram(struct w2c_env *e, u32 src, u32 dst) { (void)e; bios_LZ77UnComp(src, dst); }
void w2c_env_RLUnCompWram(struct w2c_env *e, u32 src, u32 dst) { (void)e; bios_RLUnComp(src, dst); }
void w2c_env_RLUnCompVram(struct w2c_env *e, u32 src, u32 dst) { (void)e; bios_RLUnComp(src, dst); }
void w2c_env_BgAffineSet(struct w2c_env *e, u32 src, u32 dst, u32 n) { (void)e; bios_BgAffineSet(src, dst, (int32_t)n); }
void w2c_env_ObjAffineSet(struct w2c_env *e, u32 src, u32 dst, u32 n, u32 off) { (void)e; bios_ObjAffineSet(src, dst, (int32_t)n, (int32_t)off); }
void w2c_env_irq_handler(struct w2c_env *e) { (void)e; }
void w2c_env_hal_syscall(struct w2c_env *e, u32 num) {
    (void)e;
    switch (num) {
    case 2: case 5: hal_run_frame(); break;            /* Halt / VBlankIntrWait: advance one frame */
    default: break;
    }
}

/* ---------- HAL callbacks into wasm ---------- */
static void cb_vblank(void) { w2c_wl4_InterruptCallbackCallVBlank(&g_inst); }
static void cb_hblank(void) { w2c_wl4_InterruptCallbackCallHBlank(&g_inst); }
static void cb_vcount(void) { w2c_wl4_InterruptCallbackCallVCount(&g_inst); }
static void cb_frame(void) {
    static struct timespec next; struct timespec now;
    pthread_mutex_lock(&g_lock); memcpy(g_front, hal_framebuffer(), sizeof g_front); g_frames++; pthread_mutex_unlock(&g_lock);
    if (g_frames == 1 || g_frames % 300 == 0) {
        int nz = 0; for (int i = 0; i < GBA_W * GBA_H; i++) if (g_front[i] & 0xFFFFFF) nz++;
        WL4_LOG("frame %d, non-black pixels %d/%d", g_frames, nz, GBA_W * GBA_H);
    }
    if (g_stop) { g_running = 0; pthread_exit(NULL); }
    while (g_paused && !g_stop) { struct timespec ts = {0, 16000000}; nanosleep(&ts, NULL); }
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!next.tv_sec || (now.tv_sec - next.tv_sec) * 1000000000LL + (now.tv_nsec - next.tv_nsec) > 100000000LL) next = now;
    next.tv_nsec += 16743000;                           /* 59.7275 Hz */
    while (next.tv_nsec >= 1000000000L) { next.tv_nsec -= 1000000000L; next.tv_sec++; }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
}

static void *game_main(void *arg) {
    (void)arg;
    WL4_LOG("game thread started, rom=%zu bytes", g_romSize);
    wasm_rt_init();
    g_env.inst = &g_inst;
    wasm2c_wl4_instantiate(&g_inst, &g_env);
    hal_init(g_inst.w2c_memory.data, g_inst.w2c_memory.size);
    HalCallbacks cb = { cb_vblank, cb_hblank, cb_vcount, cb_frame }; hal_set_callbacks(&cb);
    if (g_haveSave) memcpy(hal_sram(), g_save, GBA_SRAM_SIZE);
    if (hal_load_rom(g_rom, g_romSize, g_inst.w2c_rom_image) != 0) { WL4_LOG("ROM does not fit (size=%zu)", g_romSize); g_running = 0; return NULL; }
    WL4_LOG("ROM loaded, entering AgbMain");
    w2c_wl4_AgbMain(&g_inst);                           /* never returns while the game runs */
    WL4_LOG("AgbMain returned, game thread exits");
    g_running = 0; return NULL;
}

int wl4_start(const uint8_t *rom, size_t size) {
    if (g_running) return -1;
    free(g_rom); g_rom = malloc(size); if (!g_rom) return -2; memcpy(g_rom, rom, size); g_romSize = size;
    g_stop = 0; g_paused = 0; g_frames = 0; g_running = 1;
    WL4_LOG("wl4_start: rom=%zu bytes", size);
    return pthread_create(&g_thread, NULL, game_main, NULL) ? -3 : 0;
}
#ifdef __ANDROID__
/* bionic has no pthread_cancel(): kill the stuck game thread with a signal whose handler exits the thread */
static void wl4_kill_handler(int sig) { (void)sig; g_running = 0; pthread_exit(NULL); }
static void wl4_force_stop(void) {
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = wl4_kill_handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    pthread_kill(g_thread, SIGUSR2);
}
#else
static void wl4_force_stop(void) { pthread_cancel(g_thread); }
#endif
void wl4_stop(void) {
    g_stop = 1;
    for (int i = 0; i < 100 && g_running; i++) { struct timespec ts = {0, 10000000}; nanosleep(&ts, NULL); }
    if (g_running) wl4_force_stop();                    /* game stuck in a loop that never reaches a frame boundary */
    pthread_join(g_thread, NULL); g_running = 0;
}
void wl4_pause(int p) { g_paused = p; }
void wl4_set_keys(uint16_t k) { hal_set_keys(k); }
void wl4_load_save(const uint8_t *d, size_t n) { memset(g_save, 0xFF, sizeof g_save); memcpy(g_save, d, n > sizeof g_save ? sizeof g_save : n); g_haveSave = 1; }
int  wl4_read_save(uint8_t *out) { if (g_running && g_inst.w2c_memory.data) memcpy(out, hal_sram(), GBA_SRAM_SIZE); else memcpy(out, g_save, GBA_SRAM_SIZE); return GBA_SRAM_SIZE; }
int  wl4_running(void) { return g_running; }
int  wl4_copy_frame(uint32_t *out) { pthread_mutex_lock(&g_lock); memcpy(out, g_front, sizeof g_front); int f = g_frames; pthread_mutex_unlock(&g_lock); return f; }
