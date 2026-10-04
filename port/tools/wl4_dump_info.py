#!/usr/bin/env python3
"""Decode a wl4_stall_*.bin snapshot written by the native side on a stall / wasm trap.
usage: wl4_dump_info.py dump.bin [--extract DIR]   (--extract writes ewram/iwram/io/pal/vram/oam .bin files)"""
import struct, sys, os
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'WL4D', 'not a WL4D dump'
n, frame = struct.unpack_from('<II', d, 4); o = 12; R = {}
for _ in range(n):
    base, ln = struct.unpack_from('<II', d, o); o += 8; R[base] = d[o:o+ln]; o += ln
iw, io = R[0x03000000], R[0x04000000]
u8 = lambda a: iw[a - 0x03000000]; u16 = lambda a: iw[a-0x03000000] | iw[a-0x03000000+1] << 8
r16 = lambda off: io[off] | io[off+1] << 8
print(f'frame {frame}')
print(f'minigame: sel={u8(0x030047b8)} state={u8(0x030047b9)} sub={u8(0x030047ba)} seq={u8(0x030047bb)} wait={u16(0x030047c6)} score={u16(0x030047d6)}')
print(f'homerun : result={u8(0x03004a2d)} miss={u8(0x03004a2e)} level={u8(0x03004a2f)}')
print(f'IO      : DISPCNT={r16(0):04x} DISPSTAT={r16(4):04x} VCOUNT={r16(6)} IE={r16(0x200):04x} IF={r16(0x202):04x} IME={r16(0x208)&1}')
for ch in range(4):
    b = 0xB0 + 12*ch; src, dst, cnt = struct.unpack_from('<III', io, b)
    print(f'DMA{ch}   : src={src:08x} dst={dst:08x} cnt/ctl={cnt:08x}')
if len(sys.argv) > 3 and sys.argv[2] == '--extract':
    os.makedirs(sys.argv[3], exist_ok=True)
    for base, name in ((0x02000000,'ewram'),(0x03000000,'iwram'),(0x04000000,'io'),(0x05000000,'pal'),(0x06000000,'vram'),(0x07000000,'oam')):
        open(os.path.join(sys.argv[3], name + '.bin'), 'wb').write(R[base])
