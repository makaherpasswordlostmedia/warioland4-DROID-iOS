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
#include <stdarg.h>
#include <unistd.h>
/* stderr is piped into logcat + the log file by jni.c (JNI_OnLoad), so plain fprintf is enough on every platform */
#define WL4_LOG(...) wl4_logf(0, "wl4: " __VA_ARGS__)   /* file + logcat + flight-recorder ring */
#define WL4_RING(...) wl4_logf(1, __VA_ARGS__)           /* flight-recorder ring only (noisy stuff) */

struct w2c_env { w2c_wl4 *inst; };

/* Last wasm functions entered (filled by the FUNC_PROLOGUE hook that port/tools/patch_w2c_trace.py adds to wl4.c). */
const char *volatile g_wl4_trace[16]; volatile unsigned g_wl4_trace_i;
extern volatile int g_hal_stage, g_hal_line; extern volatile unsigned g_hal_irq_calls[3];

static w2c_wl4 g_inst;
static struct w2c_env g_env;
static pthread_t g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_front[GBA_W * GBA_H];
static volatile int g_stop, g_running, g_frames;
static uint8_t *g_rom; static size_t g_romSize;
static int g_paused;
static volatile unsigned g_polls, g_syscalls, g_dmas;   /* diagnostics for the watchdog */
static uint8_t g_save[GBA_SRAM_SIZE]; static int g_haveSave;

/* ---------- flight recorder ----------
 * Every WL4_LOG line AND the noisy ones (WL4_RING) land in a ring of the last RING_N events.  The ring is dumped
 * into the log on a stall, a wasm trap or a native crash, so the log file itself can stay quiet. */
#define RING_N 160
static char g_ring[RING_N][160]; static volatile unsigned g_ringI;
static long long g_t0;
static long long mono_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
static void wl4_logf(int ringOnly, const char *fmt, ...) {
    char buf[300]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    unsigned i = __sync_fetch_and_add(&g_ringI, 1) % RING_N;
    long long ms = mono_ms() - g_t0;
    snprintf(g_ring[i], sizeof g_ring[i], "+%lld.%03llds f%d %s", ms / 1000, ms % 1000, g_frames, buf);
    if (!ringOnly) fprintf(stderr, "%s\n", buf);
}
/* async-signal-safe-ish: plain write(2) of the ring, oldest first (also called from the SIGSEGV handler in jni.c) */
void wl4_ring_write(int fd) {
    static const char hdr[] = "---- flight recorder (oldest first) ----\n", ftr[] = "---- end flight recorder ----\n";
    if (fd < 0) return;
    (void)!write(fd, hdr, sizeof hdr - 1);
    for (unsigned k = 0; k < RING_N; k++) {
        const char *l = g_ring[(g_ringI + k) % RING_N]; if (!l[0]) continue;
        (void)!write(fd, "  | ", 4); (void)!write(fd, l, strlen(l)); (void)!write(fd, "\n", 1);
    }
    (void)!write(fd, ftr, sizeof ftr - 1);
}

/* ---------- game-state snapshot (fixed IWRAM addresses from linker.ld) ---------- */
static void snap_game(char *buf, size_t n) {
    if (!g_mem) { snprintf(buf, n, "(no memory yet)"); return; }
    snprintf(buf, n, "minigame sel=%u st=%u sub=%u seq=%u wait=%u score=%u hr[res=%u miss=%u lvl=%u] DISPCNT=%04x STAT=%04x VC=%u IE=%04x IF=%04x IME=%u",
        rd8(0x030047b8), rd8(0x030047b9), rd8(0x030047ba), rd8(0x030047bb), rd16(0x030047c6), rd16(0x030047d6),
        rd8(0x03004a2d), rd8(0x03004a2e), rd8(0x03004a2f),
        rd16(IO_DISPCNT), rd16(IO_DISPSTAT), rd16(IO_VCOUNT), rd16(IO_IE), rd16(IO_IF), rd16(IO_IME) & 1);
}
static volatile unsigned g_lastDma[5];                  /* ch, src, dst, ctl, count */
static char g_dumpPath[256];
void wl4_set_dump_path(const char *p) { snprintf(g_dumpPath, sizeof g_dumpPath, "%s", p); }
/* EWRAM + IWRAM + IO + PAL + VRAM + OAM -> file; decode with port/tools/wl4_dump_info.py */
static void write_state_dump(void) {
    static const uint32_t reg[6][2] = {{0x02000000,0x40000},{0x03000000,0x8000},{0x04000000,0x400},{0x05000000,0x400},{0x06000000,0x18000},{0x07000000,0x400}};
    if (!g_dumpPath[0] || !g_mem) return;
    FILE *f = fopen(g_dumpPath, "wb"); if (!f) { WL4_LOG("state dump: cannot open %s", g_dumpPath); return; }
    uint32_t n = 6, fr = (uint32_t)g_frames; fwrite("WL4D", 1, 4, f); fwrite(&n, 4, 1, f); fwrite(&fr, 4, 1, f);
    for (int i = 0; i < 6; i++) { fwrite(&reg[i][0], 4, 1, f); fwrite(&reg[i][1], 4, 1, f); fwrite(g_mem + reg[i][0], 1, reg[i][1], f); }
    fclose(f); WL4_LOG("state dump written: %s (frame %u)", g_dumpPath, fr);
}

/* A wasm trap (OOB, bad indirect call...) means a port bug: report it and stop the game thread
 * instead of crashing the app.  Build the wasm2c C files with -DWASM_RT_TRAP_HANDLER=wl4_trap. */
static void dump_trace(void) {
    char buf[512]; int o = 0; unsigned ti = g_wl4_trace_i;
    for (int i = 0; i < 16 && o < (int)sizeof buf - 64; i++) { const char *nm = g_wl4_trace[(ti - 1 - i) & 15]; if (nm) o += snprintf(buf + o, sizeof buf - (size_t)o, "%s%s", i ? " < " : "", nm); }
    WL4_LOG("  last wasm funcs: %s", o ? buf : "(trace hook not compiled in)");
}
void wl4_trap(wasm_rt_trap_t code) {
    const char *nm = code == WASM_RT_TRAP_OOB ? "out-of-bounds memory access" : code == WASM_RT_TRAP_INT_OVERFLOW ? "integer overflow"
                   : code == WASM_RT_TRAP_DIV_BY_ZERO ? "division by zero" : code == WASM_RT_TRAP_INVALID_CONVERSION ? "invalid conversion"
                   : code == WASM_RT_TRAP_UNREACHABLE ? "unreachable" : (int)code == 6 ? "call_indirect (null entry / out of range / signature mismatch)" : "other (see wasm-rt.h)";
    WL4_LOG("wasm trap %d: %s (game thread stopped)", (int)code, nm);
    dump_trace();
    { char sn[400]; snap_game(sn, sizeof sn); WL4_LOG("  state: %s", sn); }
    wl4_ring_write(STDERR_FILENO); write_state_dump();
    g_running = 0; pthread_exit(NULL);
}

/* ---------- imports the wasm module expects ---------- */
void w2c_env_hal_dma_set(struct w2c_env *e, u32 ch, u32 src, u32 dst, u32 ctl) { (void)e; g_dmas++; g_lastDma[0] = ch; g_lastDma[1] = src; g_lastDma[2] = dst; g_lastDma[3] = ctl; g_lastDma[4] = g_dmas; hal_dma_set((int)ch, src, dst, ctl); }
void w2c_env_LZ77UnCompVram(struct w2c_env *e, u32 src, u32 dst) { (void)e; bios_LZ77UnComp(src, dst); }
/* Diagnostics from the wasm side: logs a value whenever it changes (tag 1 = Wario normal pose index). */
void w2c_env_hal_trace_val(struct w2c_env *e, u32 tag, u32 val) {
    static u32 last[8]; static int init;
    (void)e; if (!init) { memset(last, 0xFF, sizeof last); init = 1; }
    if (tag < 8 && last[tag] != val) { last[tag] = val; if (tag == 1) WL4_RING("pose=%u", val); else WL4_LOG("trace[%u] = %u (frame %d)", tag, val, g_frames); }
}
static char g_siteSeen[8192];
void w2c_env_hal_unported_asm(struct w2c_env *e, u32 site) {
    (void)e; if (site < sizeof g_siteSeen && !g_siteSeen[site]) { g_siteSeen[site] = 1; WL4_LOG("UNPORTED inline asm site #%u executed (see ASM_SITES.txt)", site); }
}
u32 w2c_env_hal_poll_vcount(struct w2c_env *e) { (void)e; g_polls++; return hal_poll_vcount(); }
void w2c_env_CPUSet(struct w2c_env *e, u32 src, u32 dst, u32 ctl) { (void)e; bios_CpuSet(src, dst, ctl); }
void w2c_env_CpuFastSet(struct w2c_env *e, u32 src, u32 dst, u32 ctl) { (void)e; bios_CpuFastSet(src, dst, ctl); }
void w2c_env_LZ77UnCompWram(struct w2c_env *e, u32 src, u32 dst) { (void)e; bios_LZ77UnComp(src, dst); }
void w2c_env_RLUnCompWram(struct w2c_env *e, u32 src, u32 dst) { (void)e; bios_RLUnComp(src, dst); }
void w2c_env_RLUnCompVram(struct w2c_env *e, u32 src, u32 dst) { (void)e; bios_RLUnComp(src, dst); }
void w2c_env_BgAffineSet(struct w2c_env *e, u32 src, u32 dst, u32 n) { (void)e; bios_BgAffineSet(src, dst, (int32_t)n); }
void w2c_env_ObjAffineSet(struct w2c_env *e, u32 src, u32 dst, u32 n, u32 off) { (void)e; bios_ObjAffineSet(src, dst, (int32_t)n, (int32_t)off); }
void w2c_env_irq_handler(struct w2c_env *e) { (void)e; }
void w2c_env_hal_syscall(struct w2c_env *e, u32 num) {
    (void)e; g_syscalls++;
    switch (num) {
    case 2: case 5: hal_run_frame(); break;            /* Halt / VBlankIntrWait: advance one frame */
    default: break;
    }
}

/* ---------- HAL callbacks into wasm ---------- */
static void cb_vblank(void) {
    static int n; if (n < 3) WL4_LOG("vblank callback #%d: enter (frame %d)", n + 1, g_frames);
    w2c_wl4_InterruptCallbackCallVBlank(&g_inst);
    if (n < 3) WL4_LOG("vblank callback #%d: done", n + 1);
    n++;
}
static void cb_hblank(void) { w2c_wl4_InterruptCallbackCallHBlank(&g_inst); }
static void cb_vcount(void) { w2c_wl4_InterruptCallbackCallVCount(&g_inst); }
static void cb_frame(void) {
    static struct timespec next; struct timespec now;
    pthread_mutex_lock(&g_lock); memcpy(g_front, hal_framebuffer(), sizeof g_front); g_frames++; pthread_mutex_unlock(&g_lock);
    if (g_mem) {                                          /* minigame state machine changes (rate limited, then ring only) */
        static unsigned char lastMg[4]; static int nlog;
        unsigned char cur[4] = { rd8(0x030047b8), rd8(0x030047b9), rd8(0x030047ba), rd8(0x030047bb) };
        if (memcmp(cur, lastMg, 4)) { memcpy(lastMg, cur, 4);
            if (nlog++ < 300) WL4_LOG("minigame sel=%u st=%u sub=%u seq=%u (frame %d)", cur[0], cur[1], cur[2], cur[3], g_frames);
            else WL4_RING("minigame sel=%u st=%u sub=%u seq=%u", cur[0], cur[1], cur[2], cur[3]); }
    }
    if (g_frames <= 3 || g_frames == 10 || g_frames == 30 || g_frames % 600 == 0) {
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

/* One tick per second.  Normal play: quiet (file gets a line every 30 s, the ring gets one every second).
 * STALL (frames not advancing, not paused): immediately log what the game was doing, after 2 s dump the flight
 * recorder + a full memory snapshot file, and keep reporting every 10 s.  "recovered" is logged when frames resume. */
static void *watchdog(void *arg) {
    (void)arg; unsigned lastFrames = (unsigned)-1, lastTraceI = 0; int stallSecs = 0;
    for (int t = 1; g_running && !g_stop; t++) {
        struct timespec ts = {1, 0}; nanosleep(&ts, NULL);
        if (!g_running || g_stop) break;
        unsigned f = (unsigned)g_frames, ti = g_wl4_trace_i; int stalled = (f == lastFrames) && !g_paused;
        char sn[400]; snap_game(sn, sizeof sn);
        if (t <= 5 || t % 30 == 0 || stalled) WL4_LOG("alive t=%ds frames=%u%s polls=%u syscalls=%u dma=%u | %s", t, f, stalled ? " (STALLED)" : "", g_polls, g_syscalls, g_dmas, sn);
        else WL4_RING("alive t=%ds frames=%u | %s", t, f, sn);
        if (stalled) {
            stallSecs++;
            if (stallSecs == 1 || stallSecs % 10 == 0) {
                WL4_LOG("  stall #%ds: hal line=%d stage=%d irq(v/h/c)=%u/%u/%u wasm calls in last 1s=%u (%s)", stallSecs, g_hal_line, g_hal_stage,
                        g_hal_irq_calls[0], g_hal_irq_calls[1], g_hal_irq_calls[2], ti - lastTraceI, ti != lastTraceI ? "loop that keeps calling functions" : "tight loop, no calls");
                WL4_LOG("  last DMA: #%u ch%u src=%08x dst=%08x ctl=%08x", g_lastDma[4], g_lastDma[0], g_lastDma[1], g_lastDma[2], g_lastDma[3]);
                dump_trace();
            }
            if (stallSecs == 2) { wl4_ring_write(STDERR_FILENO); write_state_dump(); }
        } else if (stallSecs) { WL4_LOG("recovered after %ds stall (frame %u)", stallSecs, f); stallSecs = 0; }
        lastFrames = f; lastTraceI = ti;
    }
    return NULL;
}

int wl4_start(const uint8_t *rom, size_t size) {
    if (g_running) return -1;
    free(g_rom); g_rom = malloc(size); if (!g_rom) return -2; memcpy(g_rom, rom, size); g_romSize = size;
    g_stop = 0; g_paused = 0; g_frames = 0; g_t0 = mono_ms(); memset(g_ring, 0, sizeof g_ring); g_ringI = 0; g_polls = g_syscalls = g_dmas = 0; g_running = 1;
    WL4_LOG("wl4_start: rom=%zu bytes", size);
    if (pthread_create(&g_thread, NULL, game_main, NULL)) return -3;
    pthread_t wd; if (pthread_create(&wd, NULL, watchdog, NULL) == 0) pthread_detach(wd);
    return 0;
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
