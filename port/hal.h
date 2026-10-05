/* GBA hardware abstraction layer for the Wario Land 4 native port.
 *
 * The decomp is compiled to wasm32 (so pointers/struct layouts match the GBA),
 * converted to C with wasm2c, and run against one linear memory.  The GBA
 * address space (EWRAM/IWRAM/IO/PAL/VRAM/OAM/ROM) lives at its *real* addresses
 * inside that memory, so the game's hardware pokes (*(vu16*)0x4000000 ...)
 * are ordinary loads/stores.  This HAL reads those regions to produce video
 * and audio, and implements the BIOS calls. */
#ifndef WL4_HAL_H
#define WL4_HAL_H
#include <stdint.h>
#include <stddef.h>

#define GBA_W 240
#define GBA_H 160
#define GBA_ROM_BASE 0x08000000u
#define GBA_SRAM_BASE 0x0E000000u
#define GBA_SRAM_SIZE 0x10000u
#define GBA_MEM_MIN  0x0E010000u   /* linear memory must cover SRAM */

/* ---- core ---- */
void hal_init(uint8_t *mem, size_t memSize);          /* mem = wasm linear memory */
int  hal_load_rom(const uint8_t *rom, size_t size, uint32_t rom_image_addr);   /* ROM -> 0x08000000 and rom_image */
void hal_set_keys(uint16_t pressed);                  /* active-high: bit0=A ... bit9=L (KEYINPUT order) */
void hal_run_frame(void);                             /* finish the current frame */
void hal_step_line(void);                             /* advance one scanline */
uint16_t hal_poll_vcount(void);                       /* game busy-waits on REG_VCOUNT */                             /* 228 scanlines, fires IRQ callbacks */
uint8_t *hal_sram(void);                              /* 64KB battery save at 0x0E000000 */
const uint32_t *hal_framebuffer(void);                /* 240x160 XRGB8888 */

/* callbacks into the game (wired to the wasm2c exports by the glue) */
typedef struct {
    void (*vblank)(void);
    void (*hblank)(void);
    void (*vcount)(void);
    void (*frame_done)(void);                         /* host: present + pace + audio */
} HalCallbacks;
void hal_set_callbacks(const HalCallbacks *cb);

/* ---- memory helpers (little endian, bounds are the caller's job) ---- */
extern uint8_t *g_mem;
static inline uint8_t  rd8 (uint32_t a){ return g_mem[a]; }
static inline uint16_t rd16(uint32_t a){ return (uint16_t)(g_mem[a] | g_mem[a+1] << 8); }
static inline uint32_t rd32(uint32_t a){ return rd16(a) | (uint32_t)rd16(a+2) << 16; }
static inline void wr8 (uint32_t a, uint8_t v){ g_mem[a]=v; }
static inline void wr16(uint32_t a, uint16_t v){ g_mem[a]=(uint8_t)v; g_mem[a+1]=(uint8_t)(v>>8); }
static inline void wr32(uint32_t a, uint32_t v){ wr16(a,(uint16_t)v); wr16(a+2,(uint16_t)(v>>16)); }

/* ---- IO register addresses ---- */
enum {
    IO_DISPCNT=0x04000000, IO_DISPSTAT=0x04000004, IO_VCOUNT=0x04000006,
    IO_BG0CNT=0x04000008, IO_BG0HOFS=0x04000010, IO_BG2PA=0x04000020, IO_BG2X=0x04000028,
    IO_BG3PA=0x04000030, IO_BG3X=0x04000038,
    IO_WIN0H=0x04000040, IO_WIN1H=0x04000042, IO_WIN0V=0x04000044, IO_WIN1V=0x04000046,
    IO_WININ=0x04000048, IO_WINOUT=0x0400004A, IO_MOSAIC=0x0400004C,
    IO_BLDCNT=0x04000050, IO_BLDALPHA=0x04000052, IO_BLDY=0x04000054,
    IO_KEYINPUT=0x04000130, IO_IE=0x04000200, IO_IF=0x04000202, IO_IME=0x04000208,
    PAL_BASE=0x05000000, VRAM_BASE=0x06000000, OAM_BASE=0x07000000
};

void hal_io_write(unsigned addr, unsigned size);      /* store hook for the IO page */

/* ---- PPU ---- */
void ppu_render_scanline(int y, uint32_t *dst240);

/* ---- DMA (called through the macro overrides in port/include/gba/macro.h) ---- */
void hal_dma_set(int ch, uint32_t src, uint32_t dst, uint32_t cnt_ctrl);   /* cnt | ctl<<16 */

/* ---- BIOS HLE (also exported to the wasm module as imports) ---- */
void     bios_CpuSet(uint32_t src, uint32_t dst, uint32_t ctl);
void     bios_CpuFastSet(uint32_t src, uint32_t dst, uint32_t ctl);
void     bios_LZ77UnComp(uint32_t src, uint32_t dst);
void     bios_RLUnComp(uint32_t src, uint32_t dst);
int32_t  bios_Div(int32_t num, int32_t den, int32_t *mod, uint32_t *absResult);
uint16_t bios_Sqrt(uint32_t v);
uint16_t bios_ArcTan(int16_t t);
uint16_t bios_ArcTan2(int16_t x, int16_t y);
void     bios_ObjAffineSet(uint32_t src, uint32_t dst, int32_t count, int32_t stride);
void     bios_BgAffineSet(uint32_t src, uint32_t dst, int32_t count);
#endif
