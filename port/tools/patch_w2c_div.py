#!/usr/bin/env python3
"""GBA division semantics for wasm2c output.

On the GBA, `/` and `%` call __divsi3 & co -> BIOS Div, which never faults: x/0 gives +-1 (x % 0 = x),
INT_MIN/-1 gives INT_MIN.  clang lowers `/` straight to wasm i32.div_s/div_u, and wasm2c turns a zero
divisor into a trap (wasm trap 3 = DIV_BY_ZERO), killing the game thread the first time decomp code like
FixedDiv()/FixedInverse() sees a zero (the original hardware shrugged and carried on).

This replaces the DIV_S/REM_S/DIV_U/REM_U macros in wl4.c with non-trapping versions.
usage: patch_w2c_div.py wl4.c
"""
import re, sys
path = sys.argv[1]
src = open(path).read()
if 'WL4_GBA_DIV' in src:
    print('patch_w2c_div: already patched'); sys.exit(0)

NEW = {
 ('DIV_S', 4): '#define DIV_S(ut, min, x, y) /*WL4_GBA_DIV*/ ((y) == 0 ? (ut)((x) < 0 ? -1 : 1) : (((x) == (min) && (y) == -1) ? (ut)(min) : (ut)((x) / (y))))',
 ('REM_S', 4): '#define REM_S(ut, min, x, y) /*WL4_GBA_DIV*/ ((y) == 0 ? (ut)(x) : (((x) == (min) && (y) == -1) ? (ut)0 : (ut)((x) % (y))))',
 ('DIV_U', 2): '#define DIV_U(x, y) /*WL4_GBA_DIV*/ ((y) == 0 ? 0xFFFFFFFFu : ((x) / (y)))',
 ('REM_U', 2): '#define REM_U(x, y) /*WL4_GBA_DIV*/ ((y) == 0 ? (x) : ((x) % (y)))',
}
lines = src.splitlines(keepends=True)
out, i, done = [], 0, []
HDR = re.compile(r'^\s*#\s*define\s+(DIV_S|REM_S|DIV_U|REM_U)\s*\(([^)]*)\)')
while i < len(lines):
    m = HDR.match(lines[i])
    if m:
        j = i
        while lines[j].rstrip().endswith('\\'): j += 1          # swallow the continuation lines
        key = (m.group(1), len([a for a in m.group(2).split(',') if a.strip()]))
        if key in NEW:
            out.append(NEW[key] + '\n'); done.append(m.group(1)); i = j + 1; continue
        print('patch_w2c_div: unexpected signature', m.group(0).strip(), '- left as is')
    out.append(lines[i]); i += 1
missing = {'DIV_S', 'REM_S', 'DIV_U', 'REM_U'} - set(done)
if missing:
    # newer wabt keeps these in wasm-rt.h / helper functions; fall back to a header-level override
    print('patch_w2c_div: WARNING macros not found in wl4.c:', sorted(missing), '- div-by-zero will still trap')
open(path, 'w').write(''.join(out))
print('patch_w2c_div: patched', sorted(done), 'in', path)
