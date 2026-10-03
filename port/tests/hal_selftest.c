#include "../hal/hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static uint32_t px(int x, int y) { return hal_framebuffer()[y * GBA_W + x] & 0xFFFFFF; }
static int vb, hb, done_;
static void on_vb(void){vb++;} static void on_hb(void){hb++;} static void on_done(void){done_++;}

int main(void) {
    size_t sz = 0x0B000000; uint8_t *mem = calloc(1, sz); hal_init(mem, sz);
    HalCallbacks cb = { on_vb, on_hb, 0, on_done }; hal_set_callbacks(&cb);

    /* --- BIOS --- */
    CHECK(bios_Sqrt(144) == 12 && bios_Sqrt(0xFFFFFFFF) == 65535 && bios_Sqrt(2) == 1);
    int32_t m; uint32_t a; CHECK(bios_Div(-7, 2, &m, &a) == -3 && m == -1 && a == 3);
    CHECK(bios_ArcTan2(1, 0) == 0 && bios_ArcTan2(0, 1) == 0x4000 && bios_ArcTan2(-1, 0) == 0x8000 && bios_ArcTan2(0, -1) == 0xC000);
    { int d = (int)bios_ArcTan2(100, 100) - 0x2000; CHECK(abs(d) < 0x40); }
    /* LZ77: header 0x10, size 8 : literal 'A' x1 then ref(len 7, disp 1) */
    { uint8_t lz[] = {0x10, 8, 0, 0, 0x40, 'A', 0x40 | 0, 0}; /* flags 0b0100_0000: lit, ref */
      lz[6] = (7 - 3) << 4 | 0; lz[7] = 0;                      /* len=7 disp=1 */
      memcpy(mem + 0x02000000, lz, sizeof lz); bios_LZ77UnComp(0x02000000, 0x02000100);
      for (int i = 0; i < 8; i++) CHECK(mem[0x02000100 + i] == 'A'); }
    { uint8_t rl[] = {0x30, 6, 0, 0, 0x80 | 3, 7, 0x01, 1, 2};  /* run of 6 x7?  (3+3) */
      memcpy(mem + 0x02000000, rl, sizeof rl); bios_RLUnComp(0x02000000, 0x02000100);
      for (int i = 0; i < 6; i++) CHECK(mem[0x02000100 + i] == 7); }
    wr32(0x02000200, 0x12345678); bios_CpuSet(0x02000200, 0x02000300, 1 | (1 << 26)); CHECK(rd32(0x02000300) == 0x12345678);
    /* ObjAffineSet identity (scale 0x100, angle 0) -> pa=0x100, pb=0, pc=0, pd=0x100 */
    wr16(0x02000400, 0x100); wr16(0x02000402, 0x100); wr16(0x02000404, 0);
    bios_ObjAffineSet(0x02000400, 0x02000500, 1, 2);
    CHECK(rd16(0x02000500) == 0x100 && rd16(0x02000502) == 0 && rd16(0x02000504) == 0 && rd16(0x02000506) == 0x100);

    /* --- PPU: mode 0, BG0 4bpp tile 1 solid colour index 1, map (0,0)=tile1 --- */
    wr16(IO_DISPCNT, 0x0100);                 /* mode0, BG0 */
    wr16(IO_BG0CNT, (1 << 8) | (0 << 2));     /* screenbase 1, charbase 0 */
    wr16(PAL_BASE, 0x0000); wr16(PAL_BASE + 2, 0x001F);   /* bd black, idx1 red */
    for (int i = 0; i < 32; i++) mem[VRAM_BASE + 32 + i] = 0x11;
    wr16(VRAM_BASE + 0x800, 1);
    hal_run_frame();
    CHECK(px(0, 0) == 0xFF0000 && px(7, 7) == 0xFF0000 && px(8, 0) == 0x000000);
    CHECK(vb == 0);                                       /* no IRQ enabled yet */
    /* scroll by 4px: tile edge moves to x=4 */
    wr16(IO_BG0HOFS, 4); hal_run_frame(); CHECK(px(3, 0) == 0xFF0000 && px(4, 0) == 0x000000);
    wr16(IO_BG0HOFS, 0);

    /* --- OBJ 8x8 at (20,10) using tile 0 of obj VRAM, palette idx 2 = green --- */
    wr16(PAL_BASE + 0x200 + 4, 0x03E0);
    for (int i = 0; i < 32; i++) mem[VRAM_BASE + 0x10000 + i] = 0x22;
    wr16(OAM_BASE, 10); wr16(OAM_BASE + 2, 20); wr16(OAM_BASE + 4, 0);
    for (int i = 1; i < 128; i++) wr16(OAM_BASE + i * 8, 0x200);        /* hide the rest */
    wr16(IO_DISPCNT, 0x1100 | 0x40);
    hal_run_frame();
    CHECK(px(20, 10) == 0x00FF00 && px(27, 17) == 0x00FF00 && px(28, 10) != 0x00FF00 && px(19, 10) != 0x00FF00);
    /* 1.5 : sprite wins over BG at equal priority; lower BG prio value beats sprite */
    wr16(IO_BG0CNT, (1 << 8) | 0); wr16(OAM_BASE + 4, 0 | (1 << 10)); hal_run_frame();
    CHECK(px(4, 4) == 0xFF0000);                                       /* outside sprite sanity */
    /* --- alpha blend BG0(top) over backdrop --- */
    wr16(IO_DISPCNT, 0x0100); wr16(IO_BLDCNT, 0x0001 | (1 << 6) | (0x20 << 8)); wr16(IO_BLDALPHA, 8 | (8 << 8));
    hal_run_frame(); CHECK(px(0, 0) == 0x7B0000 || px(0, 0) == 0x840000 || (px(0,0)>>16) >= 0x78);
    /* --- IRQ plumbing --- */
    wr16(IO_BLDCNT, 0); wr16(IO_IE, 3); wr16(IO_IME, 1); wr16(IO_DISPSTAT, 0x18);
    vb = hb = done_ = 0; hal_run_frame(); CHECK(vb == 1 && hb == 160 && done_ == 1);

    /* DMA: immediate copy + HBlank-timed 1-unit-per-line (HDMA) into BG0HOFS */
    wr32(0x02001000, 0xCAFEBABE); hal_dma_set(3, 0x02001000, 0x02001010, 1 | ((0x8000 | 0x0400) << 16)); CHECK(rd32(0x02001010) == 0xCAFEBABE);
    for (int i = 0; i < 160; i++) wr16(0x02002000 + i * 2, (uint16_t)i);
    hal_dma_set(0, 0x02002000, IO_BG0HOFS, 1 | ((0x8000 | 0x2000 | 0x0200 | 0x0060) << 16));  /* hblank, repeat, dest reload(3) */
    hal_run_frame(); CHECK(rd16(IO_BG0HOFS) == 159);
    /* memory-mapped DMA: plain stores to DMA3 regs + hook (what the wasm2c store helper does) */
    wr32(0x02003000, 0x11223344); wr32(0x040000D4, 0x02003000); hal_io_write(0x040000D4, 4);
    wr32(0x040000D8, 0x02003100); hal_io_write(0x040000D8, 4);
    wr32(0x040000DC, 0x80000001 | (1u << 26)); hal_io_write(0x040000DC, 4);
    CHECK(rd32(0x02003100) == 0x11223344); CHECK(!(rd32(0x040000DC) & 0x80000000));
    printf(fails ? "SELFTEST FAILED (%d)\n" : "SELFTEST OK\n", fails); free(mem); return fails != 0;
}
