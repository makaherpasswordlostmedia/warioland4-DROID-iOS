#include "hal.h"
#include <math.h>

void bios_CpuSet(uint32_t src, uint32_t dst, uint32_t ctl) {
    uint32_t n = ctl & 0x1FFFFF; int fixed = (ctl >> 24) & 1, w32 = (ctl >> 26) & 1;
    if (w32) { src &= ~3u; dst &= ~3u; for (uint32_t i = 0; i < n; i++) { wr32(dst + i*4, rd32(fixed ? src : src + i*4)); } }
    else     { src &= ~1u; dst &= ~1u; for (uint32_t i = 0; i < n; i++) { wr16(dst + i*2, rd16(fixed ? src : src + i*2)); } }
}
void bios_CpuFastSet(uint32_t src, uint32_t dst, uint32_t ctl) {
    uint32_t n = (ctl & 0x1FFFFF); n = (n + 7) & ~7u; int fixed = (ctl >> 24) & 1;
    src &= ~3u; dst &= ~3u;
    for (uint32_t i = 0; i < n; i++) wr32(dst + i*4, rd32(fixed ? src : src + i*4));
}
void bios_LZ77UnComp(uint32_t src, uint32_t dst) {
    uint32_t hdr = rd32(src); src += 4;
    if (((hdr >> 4) & 0xF) != 1) return;
    uint32_t size = hdr >> 8, out = 0;
    while (out < size) {
        uint8_t flags = rd8(src++);
        for (int b = 7; b >= 0 && out < size; b--) {
            if (flags & (1 << b)) {
                uint8_t b0 = rd8(src++), b1 = rd8(src++);
                uint32_t len = (b0 >> 4) + 3, disp = ((b0 & 0xF) << 8 | b1) + 1;
                while (len-- && out < size) { wr8(dst + out, rd8(dst + out - disp)); out++; }
            } else wr8(dst + out++, rd8(src++));
        }
    }
}
void bios_RLUnComp(uint32_t src, uint32_t dst) {
    uint32_t hdr = rd32(src); src += 4;
    if (((hdr >> 4) & 0xF) != 3) return;
    uint32_t size = hdr >> 8, out = 0;
    while (out < size) {
        uint8_t f = rd8(src++);
        if (f & 0x80) { uint32_t len = (f & 0x7F) + 3; uint8_t v = rd8(src++); while (len-- && out < size) wr8(dst + out++, v); }
        else          { uint32_t len = (f & 0x7F) + 1; while (len-- && out < size) wr8(dst + out++, rd8(src++)); }
    }
}
int32_t bios_Div(int32_t num, int32_t den, int32_t *mod, uint32_t *absResult) {
    if (den == 0) { if (mod) *mod = num; if (absResult) *absResult = 1; return num < 0 ? -1 : 1; }
    int32_t q = (num == INT32_MIN && den == -1) ? INT32_MIN : num / den;
    if (mod) *mod = (num == INT32_MIN && den == -1) ? 0 : num % den;
    if (absResult) *absResult = q < 0 ? (uint32_t)(-(int64_t)q) : (uint32_t)q;
    return q;
}
uint16_t bios_Sqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return (uint16_t)r;
}
uint16_t bios_ArcTan(int16_t i) {          /* BIOS polynomial, bit exact */
    int32_t a = -(((int32_t)i * i) >> 14);
    int32_t b = ((0xA9 * a) >> 14) + 0x390;
    b = ((b * a) >> 14) + 0x91C;
    b = ((b * a) >> 14) + 0xFB6;
    b = ((b * a) >> 14) + 0x16AA;
    b = ((b * a) >> 14) + 0x2081;
    b = ((b * a) >> 14) + 0x3651;
    b = ((b * a) >> 14) + 0xA2F9;
    return (uint16_t)((i * b) >> 16);
}
uint16_t bios_ArcTan2(int16_t x, int16_t y) {  /* GBATEK algorithm */
    if (y == 0) return (x >= 0) ? 0 : 0x8000;
    if (x == 0) return (y >= 0) ? 0x4000 : 0xC000;
    if (y >= 0) {
        if (x >= 0) { if (x >= y) return bios_ArcTan((int16_t)((y << 14) / x)); return 0x4000 - bios_ArcTan((int16_t)((x << 14) / y)); }
        if (-x >= y) return 0x8000 + bios_ArcTan((int16_t)((y << 14) / x)); return 0x4000 - bios_ArcTan((int16_t)((x << 14) / y));
    }
    if (x <= 0) { if (-x > -y) return 0x8000 + bios_ArcTan((int16_t)((y << 14) / x)); return 0xC000 - bios_ArcTan((int16_t)((x << 14) / y)); }
    if (x >= -y) return 0x10000 + bios_ArcTan((int16_t)((y << 14) / x)); return 0xC000 - bios_ArcTan((int16_t)((x << 14) / y));
}
static int32_t sin16(uint16_t a) { return (int32_t)lround(sin((a >> 8) * (2 * M_PI / 256.0)) * 16384.0); }
static int32_t cos16(uint16_t a) { return sin16((uint16_t)(a + 0x4000)); }

void bios_ObjAffineSet(uint32_t src, uint32_t dst, int32_t count, int32_t stride) {
    for (int i = 0; i < count; i++, src += 8, dst += stride * 4) {
        int32_t sx = (int16_t)rd16(src), sy = (int16_t)rd16(src + 2); uint16_t th = rd16(src + 4);
        int32_t s = sin16(th), c = cos16(th);
        wr16(dst,              (uint16_t)((c * sx) >> 14));
        wr16(dst + stride,     (uint16_t)((-s * sx) >> 14));
        wr16(dst + stride * 2, (uint16_t)((s * sy) >> 14));
        wr16(dst + stride * 3, (uint16_t)((c * sy) >> 14));
    }
}
void bios_BgAffineSet(uint32_t src, uint32_t dst, int32_t count) {
    for (int i = 0; i < count; i++, src += 20, dst += 16) {
        int32_t ox = (int32_t)rd32(src), oy = (int32_t)rd32(src + 4);
        int32_t cx = (int16_t)rd16(src + 8), cy = (int16_t)rd16(src + 10);
        int32_t sx = (int16_t)rd16(src + 12), sy = (int16_t)rd16(src + 14); uint16_t th = rd16(src + 16);
        int32_t s = sin16(th), c = cos16(th);
        int32_t pa = (c * sx) >> 14, pb = (-s * sx) >> 14, pc = (s * sy) >> 14, pd = (c * sy) >> 14;
        wr16(dst, (uint16_t)pa); wr16(dst + 2, (uint16_t)pb); wr16(dst + 4, (uint16_t)pc); wr16(dst + 6, (uint16_t)pd);
        wr32(dst + 8,  (uint32_t)(ox - (pa * cx + pb * cy)));
        wr32(dst + 12, (uint32_t)(oy - (pc * cx + pd * cy)));
    }
}
