/* Scanline software PPU: modes 0-5, text/affine BG, regular+affine OBJ,
 * WIN0/WIN1/OBJWIN, alpha + brightness blending.  (Mosaic: TODO.) */
#include "hal.h"
#include <string.h>

#define TRANSP 0xFFFFFFFFu
static uint32_t affRefX[2], affRefY[2];          /* BG2/BG3 internal reference points */

void ppu_frame_start(void) {
    affRefX[0] = rd32(IO_BG2X) & 0x0FFFFFFF;  affRefY[0] = rd32(IO_BG2X + 4) & 0x0FFFFFFF;
    affRefX[1] = rd32(IO_BG3X) & 0x0FFFFFFF;  affRefY[1] = rd32(IO_BG3X + 4) & 0x0FFFFFFF;
    for (int i = 0; i < 2; i++) { if (affRefX[i] & 0x08000000) affRefX[i] |= 0xF0000000; if (affRefY[i] & 0x08000000) affRefY[i] |= 0xF0000000; }
}
static uint32_t pal(uint32_t idx) { return rd16(PAL_BASE + idx * 2) & 0x7FFF; }

static void bg_text(int n, int y, uint32_t *out) {
    uint16_t cnt = rd16(IO_BG0CNT + n * 2);
    uint32_t cbase = VRAM_BASE + ((cnt >> 2) & 3) * 0x4000, sbase = VRAM_BASE + ((cnt >> 8) & 0x1F) * 0x800;
    int bpp8 = (cnt >> 7) & 1, size = cnt >> 14;
    int hofs = rd16(IO_BG0HOFS + n * 4) & 0x1FF, vofs = rd16(IO_BG0HOFS + n * 4 + 2) & 0x1FF;
    int wmask = (size & 1) ? 511 : 255, hmask = (size & 2) ? 511 : 255;
    int py = (y + vofs) & hmask;
    for (int x = 0; x < GBA_W; x++) {
        int px = (x + hofs) & wmask, tx = px >> 3, ty = py >> 3;
        int sb = (size == 1) ? (tx >> 5) : (size == 2) ? (ty >> 5) : (size == 3) ? ((tx >> 5) + ((ty >> 5) << 1)) : 0;
        uint16_t e = rd16(sbase + sb * 0x800 + (((ty & 31) << 5) + (tx & 31)) * 2);
        int ix = px & 7, iy = py & 7; if (e & 0x400) ix = 7 - ix; if (e & 0x800) iy = 7 - iy;
        uint32_t tile = e & 0x3FF, c;
        if (bpp8) { uint32_t a = cbase + tile * 64 + iy * 8 + ix; if (a >= VRAM_BASE + 0x10000) { out[x] = TRANSP; continue; } c = rd8(a); out[x] = c ? pal(c) : TRANSP; }
        else { uint32_t a = cbase + tile * 32 + iy * 4 + (ix >> 1); if (a >= VRAM_BASE + 0x10000) { out[x] = TRANSP; continue; } c = (rd8(a) >> ((ix & 1) * 4)) & 15; out[x] = c ? pal((e >> 12) * 16 + c) : TRANSP; }
    }
}
static void bg_affine(int n, int mode_slot, uint32_t *out) {      /* n = 2 or 3; mode_slot 0/1 */
    uint16_t cnt = rd16(IO_BG0CNT + n * 2);
    uint32_t cbase = VRAM_BASE + ((cnt >> 2) & 3) * 0x4000, sbase = VRAM_BASE + ((cnt >> 8) & 0x1F) * 0x800;
    int dim = 128 << (cnt >> 14), wrap = (cnt >> 13) & 1;
    uint32_t reg = n == 2 ? IO_BG2PA : IO_BG3PA;
    int32_t pa = (int16_t)rd16(reg), pb = (int16_t)rd16(reg + 2), pc = (int16_t)rd16(reg + 4), pd = (int16_t)rd16(reg + 6);
    int32_t X = (int32_t)affRefX[mode_slot], Y = (int32_t)affRefY[mode_slot];
    for (int x = 0; x < GBA_W; x++, X += pa, Y += pc) {
        int32_t u = X >> 8, v = Y >> 8;
        if (wrap) { u &= dim - 1; v &= dim - 1; } else if (u < 0 || v < 0 || u >= dim || v >= dim) { out[x] = TRANSP; continue; }
        uint8_t t = rd8(sbase + (v >> 3) * (dim >> 3) + (u >> 3));
        uint8_t c = rd8(cbase + t * 64 + (v & 7) * 8 + (u & 7));
        out[x] = c ? pal(c) : TRANSP;
    }
    affRefX[mode_slot] += pb; affRefY[mode_slot] += pd;
}
static void bg_bitmap(int mode, int disp, int y, uint32_t *out) {
    uint32_t page = (disp & 0x10) ? 0xA000 : 0;
    int32_t pa = (int16_t)rd16(IO_BG2PA), pb = (int16_t)rd16(IO_BG2PA + 2), pc = (int16_t)rd16(IO_BG2PA + 4), pd = (int16_t)rd16(IO_BG2PA + 6);
    int32_t X = (int32_t)affRefX[0], Y = (int32_t)affRefY[0];
    int w = mode == 5 ? 160 : 240, h = mode == 5 ? 128 : 160;
    for (int x = 0; x < GBA_W; x++, X += pa, Y += pc) {
        int32_t u = X >> 8, v = Y >> 8;
        if (u < 0 || v < 0 || u >= w || v >= h) { out[x] = TRANSP; continue; }
        if (mode == 4) { uint8_t c = rd8(VRAM_BASE + page + v * 240 + u); out[x] = c ? pal(c) : TRANSP; }
        else out[x] = rd16(VRAM_BASE + (mode == 5 ? page : 0) + (v * w + u) * 2) & 0x7FFF;
    }
    affRefX[0] += pb; affRefY[0] += pd; (void)y;
}

static const uint8_t OBJW[3][4] = {{8,16,32,64},{16,32,32,64},{8,8,16,32}};
static const uint8_t OBJH[3][4] = {{8,16,32,64},{8,8,16,32},{16,32,32,64}};

/* obj line: color | prio<<16 | flags (bit24 semi-transparent) ; objwin mask separately */
static void obj_line(int y, int disp, uint32_t *col, uint8_t *prio, uint8_t *semi, uint8_t *objwin) {
    for (int x = 0; x < GBA_W; x++) { col[x] = TRANSP; prio[x] = 4; semi[x] = 0; objwin[x] = 0; }
    int oneD = (disp >> 6) & 1; uint32_t tbase = VRAM_BASE + 0x10000; int bitmapMode = (disp & 7) >= 3;
    for (int i = 0; i < 128; i++) {
        uint32_t o = OAM_BASE + i * 8; uint16_t a0 = rd16(o), a1 = rd16(o + 2), a2 = rd16(o + 4);
        int affine = (a0 >> 8) & 1, dbl = affine && ((a0 >> 9) & 1);
        if (!affine && ((a0 >> 9) & 1)) continue;
        int mode = (a0 >> 10) & 3; if (mode == 3) continue;
        int shape = a0 >> 14, sz = a1 >> 14; if (shape == 3) continue;
        int w = OBJW[shape][sz], h = OBJH[shape][sz], bw = dbl ? w * 2 : w, bh = dbl ? h * 2 : h;
        int oy = a0 & 0xFF, ox = a1 & 0x1FF; if (ox >= 256) ox -= 512;
        int ly = (y - oy) & 0xFF; if (ly >= bh) continue;
        int bpp8 = (a0 >> 13) & 1, tile = a2 & 0x3FF, p = (a2 >> 10) & 3, pb4 = a2 >> 12;
        if (bitmapMode && tile < 512) continue;
        int32_t pa = 0x100, pbb = 0, pc = 0, pd = 0x100;
        if (affine) { uint32_t g = OAM_BASE + ((a1 >> 9) & 31) * 32; pa = (int16_t)rd16(g + 6); pbb = (int16_t)rd16(g + 14); pc = (int16_t)rd16(g + 22); pd = (int16_t)rd16(g + 30); }
        for (int sx = 0; sx < bw; sx++) {
            int x = ox + sx; if (x < 0 || x >= GBA_W) continue;
            int tx, ty;
            if (affine) { int dx = sx - bw / 2, dy = ly - bh / 2; tx = ((pa * dx + pbb * dy) >> 8) + w / 2; ty = ((pc * dx + pd * dy) >> 8) + h / 2; if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue; }
            else { tx = (a1 & 0x1000) ? w - 1 - sx : sx; ty = (a1 & 0x2000) ? h - 1 - ly : ly; }
            int tw = w >> 3, step = bpp8 ? 2 : 1;
            uint32_t tn = oneD ? tile + ((ty >> 3) * tw + (tx >> 3)) * step : tile + (ty >> 3) * 32 + (tx >> 3) * step;
            uint32_t addr = tbase + (tn & 0x3FF) * 32, c;
            if (bpp8) c = rd8(addr + (ty & 7) * 8 + (tx & 7));
            else c = (rd8(addr + (ty & 7) * 4 + ((tx & 7) >> 1)) >> (((tx & 7) & 1) * 4)) & 15;
            if (!c) continue;
            if (mode == 2) { objwin[x] = 1; continue; }
            if (p >= prio[x]) continue;               /* earlier OAM entry wins ties */
            col[x] = pal(256 + (bpp8 ? c : pb4 * 16 + c)); prio[x] = (uint8_t)p; semi[x] = (mode == 1);
        }
    }
}

static inline uint32_t blend(uint32_t a, uint32_t b, int eva, int evb) {
    int r = ((a & 31) * eva + (b & 31) * evb) >> 4, g = (((a >> 5) & 31) * eva + ((b >> 5) & 31) * evb) >> 4, bl = (((a >> 10) & 31) * eva + ((b >> 10) & 31) * evb) >> 4;
    if (r > 31) r = 31; if (g > 31) g = 31; if (bl > 31) bl = 31; return r | g << 5 | bl << 10;
}
static inline uint32_t bright(uint32_t c, int evy, int up) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    if (up) { r += ((31 - r) * evy) >> 4; g += ((31 - g) * evy) >> 4; b += ((31 - b) * evy) >> 4; }
    else    { r -= (r * evy) >> 4; g -= (g * evy) >> 4; b -= (b * evy) >> 4; }
    return r | g << 5 | b << 10;
}
static inline uint32_t to_xrgb(uint32_t c) {
    uint32_t r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    return 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
}

void ppu_render_scanline(int y, uint32_t *dst) {
    uint16_t disp = rd16(IO_DISPCNT); int mode = disp & 7;
    if (disp & 0x80) { for (int x = 0; x < GBA_W; x++) dst[x] = 0xFFFFFFFFu; return; }
    static uint32_t bg[4][GBA_W], oc[GBA_W]; static uint8_t op[GBA_W], os[GBA_W], ow[GBA_W];
    int bgOn[4] = {0,0,0,0};
    for (int n = 0; n < 4; n++) bgOn[n] = (disp >> (8 + n)) & 1;
    switch (mode) {
    case 0: for (int n = 0; n < 4; n++) if (bgOn[n]) bg_text(n, y, bg[n]); break;
    case 1: if (bgOn[0]) bg_text(0, y, bg[0]); if (bgOn[1]) bg_text(1, y, bg[1]); if (bgOn[2]) bg_affine(2, 0, bg[2]); bgOn[3] = 0; break;
    case 2: if (bgOn[2]) bg_affine(2, 0, bg[2]); if (bgOn[3]) bg_affine(3, 1, bg[3]); bgOn[0] = bgOn[1] = 0; break;
    case 3: case 4: case 5: bgOn[0] = bgOn[1] = bgOn[3] = 0; if (bgOn[2]) bg_bitmap(mode, disp, y, bg[2]); break;
    default: bgOn[0]=bgOn[1]=bgOn[2]=bgOn[3]=0; }
    int objOn = (disp >> 12) & 1;
    if (objOn) obj_line(y, disp, oc, op, os, ow); else for (int x = 0; x < GBA_W; x++) { oc[x] = TRANSP; op[x] = 4; os[x] = 0; ow[x] = 0; }

    int w0 = (disp >> 13) & 1, w1 = (disp >> 14) & 1, wo = (disp >> 15) & 1, anyWin = w0 | w1 | wo;
    uint16_t winin = rd16(IO_WININ), winout = rd16(IO_WINOUT);
    int x0a = rd16(IO_WIN0H) >> 8, x0b = rd16(IO_WIN0H) & 0xFF, y0a = rd16(IO_WIN0V) >> 8, y0b = rd16(IO_WIN0V) & 0xFF;
    int x1a = rd16(IO_WIN1H) >> 8, x1b = rd16(IO_WIN1H) & 0xFF, y1a = rd16(IO_WIN1V) >> 8, y1b = rd16(IO_WIN1V) & 0xFF;
    if (x0b > 240 || x0b < x0a) x0b = 240; if (x1b > 240 || x1b < x1a) x1b = 240;
    int in0 = w0 && y >= y0a && y < (y0b > 160 || y0b < y0a ? 160 : y0b), in1 = w1 && y >= y1a && y < (y1b > 160 || y1b < y1a ? 160 : y1b);

    uint16_t bld = rd16(IO_BLDCNT), alpha = rd16(IO_BLDALPHA); int bmode = (bld >> 6) & 3;
    int eva = alpha & 0x1F, evb = (alpha >> 8) & 0x1F, evy = rd16(IO_BLDY) & 0x1F; if (eva > 16) eva = 16; if (evb > 16) evb = 16; if (evy > 16) evy = 16;
    uint16_t bgcnt[4]; for (int n = 0; n < 4; n++) bgcnt[n] = rd16(IO_BG0CNT + n * 2);
    uint32_t backdrop = pal(0);

    for (int x = 0; x < GBA_W; x++) {
        int en = 0x3F;                                        /* bits0-3 BG, 4 OBJ, 5 effects */
        if (anyWin) {
            if (in0 && x >= x0a && x < x0b) en = winin & 0x3F;
            else if (in1 && x >= x1a && x < x1b) en = (winin >> 8) & 0x3F;
            else if (wo && ow[x]) en = (winout >> 8) & 0x3F;
            else en = winout & 0x3F;
        }
        uint32_t c1 = backdrop, c2 = backdrop; int l1 = 5, l2 = 5, got = 0, semi = 0;   /* layer 0-3 BG, 4 OBJ, 5 backdrop */
        for (int p = 0; p < 4 && got < 2; p++) {
            if ((en & 0x10) && oc[x] != TRANSP && op[x] == p) { if (!got) { c1 = oc[x]; l1 = 4; semi = os[x]; } else { c2 = oc[x]; l2 = 4; } got++; if (got == 2) break; }
            for (int n = 0; n < 4 && got < 2; n++)
                if (bgOn[n] && (en >> n & 1) && (bgcnt[n] & 3) == p && bg[n][x] != TRANSP) { if (!got) { c1 = bg[n][x]; l1 = n; } else { c2 = bg[n][x]; l2 = n; } got++; }
        }
        uint32_t out = c1;
        int first = (bld >> l1) & 1, second = (bld >> (8 + l2)) & 1;
        if (l1 == 4 && semi && second) out = blend(c1, c2, eva, evb);                         /* semi-transparent OBJ */
        else if ((en & 0x20) && first) {
            if (bmode == 1 && second) out = blend(c1, c2, eva, evb);
            else if (bmode == 2) out = bright(c1, evy, 1);
            else if (bmode == 3) out = bright(c1, evy, 0);
        }
        dst[x] = to_xrgb(out);
    }
}
