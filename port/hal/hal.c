#include "hal.h"
#include <string.h>

uint8_t *g_mem;
static size_t g_memSize;
static uint16_t g_keys;
static uint32_t g_fb[GBA_W * GBA_H];
static HalCallbacks g_cb;
void ppu_frame_start(void);
void hal_step_line(void);
static void dma_timed(int timing);

void hal_init(uint8_t *mem, size_t size) { g_mem = mem; g_memSize = size; g_keys = 0; memset(g_fb, 0, sizeof g_fb); if (size >= GBA_MEM_MIN) memset(mem + GBA_SRAM_BASE, 0xFF, GBA_SRAM_SIZE); wr16(IO_KEYINPUT, 0x03FF); }
int hal_load_rom(const uint8_t *rom, size_t size, uint32_t rom_image_addr) {
    if (size > 0x02000000 || GBA_ROM_BASE + size > g_memSize || rom_image_addr + size > g_memSize) return -1;
    memcpy(g_mem + GBA_ROM_BASE, rom, size);
    memcpy(g_mem + rom_image_addr, rom, size);
    return 0;
}
void hal_set_keys(uint16_t pressed) { g_keys = pressed & 0x03FF; }
void hal_set_callbacks(const HalCallbacks *cb) { g_cb = *cb; }
const uint32_t *hal_framebuffer(void) { return g_fb; }

static void raise_irq(uint16_t flag, void (*cb)(void)) {
    uint16_t ie = rd16(IO_IE);
    if (!(ie & flag) || !(rd16(IO_IME) & 1)) return;
    wr16(IO_IF, rd16(IO_IF) | flag);                      /* game's handler clears it in crt0's irq_handler path */
    if (cb) cb();
    wr16(IO_IF, rd16(IO_IF) & ~flag);
}

/* Time advances one scanline at a time: either because the game halts (hal_run_frame
 * finishes the frame) or because it busy-waits on VCOUNT (hal_poll_vcount). */
static int g_line;
void hal_step_line(void) {
    int line = g_line;
    if (line == 0) { ppu_frame_start(); wr16(IO_KEYINPUT, (uint16_t)(~g_keys & 0x03FF)); }
    uint16_t st = rd16(IO_DISPSTAT);
    wr16(IO_VCOUNT, (uint16_t)line);
    int vcmatch = ((st >> 8) == line);
    wr16(IO_DISPSTAT, (uint16_t)((st & ~7) | (line >= 160 && line < 227 ? 1 : 0) | (vcmatch ? 4 : 0)));
    if (vcmatch && (st & 0x20)) raise_irq(0x0004, g_cb.vcount);
    if (line < GBA_H) {
        ppu_render_scanline(line, g_fb + line * GBA_W);
        dma_timed(2);
        wr16(IO_DISPSTAT, rd16(IO_DISPSTAT) | 2);
        if (st & 0x10) raise_irq(0x0002, g_cb.hblank);
        wr16(IO_DISPSTAT, rd16(IO_DISPSTAT) & ~2);
    } else if (line == GBA_H) { dma_timed(1); if (st & 0x08) raise_irq(0x0001, g_cb.vblank); }
    g_line = line + 1;
    if (g_line == 228) { g_line = 0; if (g_cb.frame_done) g_cb.frame_done(); }
}
void hal_run_frame(void) { do hal_step_line(); while (g_line != 0); }
uint16_t hal_poll_vcount(void) { hal_step_line(); return (uint16_t)g_line; }

/* ---- DMA ------------------------------------------------------------------
 * The decomp funnels DMA through DmaSet(); port/include/gba/macro.h turns that
 * into a call to hal_dma_set().  Immediate transfers run now; VBlank/HBlank
 * timed ones are latched and replayed from hal_run_frame(). */
typedef struct { uint32_t src, dst, cnt; uint16_t ctl; int active; } DmaCh;
static DmaCh dma[4];

static void dma_xfer(DmaCh *c, uint32_t units) {
    int w32 = (c->ctl >> 10) & 1, dc = (c->ctl >> 5) & 3, sc = (c->ctl >> 7) & 3, step = w32 ? 4 : 2;
    for (uint32_t i = 0; i < units; i++) {
        if (w32) wr32(c->dst, rd32(c->src)); else wr16(c->dst, rd16(c->src));
        c->src += sc == 0 ? step : sc == 1 ? -step : 0;
        c->dst += (dc == 0 || dc == 3) ? step : dc == 1 ? -step : 0;
    }
}
static uint32_t dma_units(int ch, uint32_t cnt) { cnt &= (ch == 3) ? 0xFFFF : 0x3FFF; return cnt ? cnt : (ch == 3 ? 0x10000 : 0x4000); }

void hal_dma_set(int ch, uint32_t src, uint32_t dst, uint32_t cntctl) {
    DmaCh *c = &dma[ch]; c->src = src; c->dst = dst; c->cnt = cntctl & 0xFFFF; c->ctl = (uint16_t)(cntctl >> 16);
    if (!(c->ctl & 0x8000)) { c->active = 0; return; }
    int timing = (c->ctl >> 12) & 3;
    if (timing == 0) {
        dma_xfer(c, dma_units(ch, c->cnt)); c->ctl &= ~0x8000; c->active = 0;
        wr16(0x040000B0 + 12 * ch + 10, c->ctl);          /* busy-wait loops see the enable bit drop */
    }
    else if (timing == 3) c->active = 0;                 /* sound FIFO DMA: handled by the audio core */
    else c->active = timing;                             /* 1 = VBlank, 2 = HBlank */
}
static void dma_timed(int timing) {
    for (int i = 0; i < 4; i++) {
        DmaCh *c = &dma[i]; if (c->active != timing) continue;
        uint32_t savedDst = c->dst;
        dma_xfer(c, timing == 2 ? (c->cnt ? c->cnt : 1) : dma_units(i, c->cnt));
        if (timing == 2 && ((c->ctl >> 5) & 3) == 3) c->dst = savedDst;
        if (!((c->ctl >> 9) & 1)) { c->active = 0; c->ctl &= ~0x8000; }
        else if (((c->ctl >> 5) & 3) == 3 && timing == 1) c->dst = savedDst;
    }
}

uint8_t *hal_sram(void) { return g_mem + GBA_SRAM_BASE; }

/* Called by the wasm2c store helper for every store into 0x04000000..0x040003FF. */
void hal_io_write(unsigned addr, unsigned size) {
    if (addr >= 0x040000B0 && addr < 0x040000E0) {
        unsigned off = (addr - 0x040000B0) % 12, ch = (addr - 0x040000B0) / 12, base = 0x040000B0 + ch * 12;
        if (off + size > 10) hal_dma_set((int)ch, rd32(base), rd32(base + 4), rd32(base + 8));
    }
}
