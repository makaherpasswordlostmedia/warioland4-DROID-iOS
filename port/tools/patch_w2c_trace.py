#!/usr/bin/env python3
"""Diagnostics: make every wasm2c function record its name in a 16-entry ring (g_wl4_trace, see host.c).
The watchdog prints it when the frame counter stalls, which names the function that contains the hang.
Active only when the library is compiled with -DWL4_TRACE (android CMakeLists does).  Non-fatal if the
FUNC_PROLOGUE macro is not found (other wabt versions): tracing is simply skipped.

usage: patch_w2c_trace.py wl4.c
"""
import re, sys
path = sys.argv[1]
src = open(path).read()
if 'WL4_ORIG_FUNC_PROLOGUE' in src:
    print('patch_w2c_trace: already patched'); sys.exit(0)
lines = src.splitlines(keepends=True)
DEF = re.compile(r'^(\s*#\s*define\s+)FUNC_PROLOGUE\b')
defs = [i for i, l in enumerate(lines) if DEF.match(l)]
use = next((i for i, l in enumerate(lines) if 'FUNC_PROLOGUE' in l and not DEF.match(l) and not l.lstrip().startswith('#')), None)
if not defs or use is None:
    print('patch_w2c_trace: FUNC_PROLOGUE not found - tracing disabled'); sys.exit(0)
for i in defs:
    lines[i] = DEF.sub(r'\1WL4_ORIG_FUNC_PROLOGUE', lines[i], count=1)
hook = (
    '#ifdef WL4_TRACE\n'
    'extern const char *volatile g_wl4_trace[16]; extern volatile unsigned g_wl4_trace_i;\n'
    '#define FUNC_PROLOGUE do { g_wl4_trace[g_wl4_trace_i++ & 15] = __func__; WL4_ORIG_FUNC_PROLOGUE; } while (0)\n'
    '#else\n'
    '#define FUNC_PROLOGUE WL4_ORIG_FUNC_PROLOGUE\n'
    '#endif\n')
lines.insert(use, hook)
open(path, 'w').write(''.join(lines))
print('patch_w2c_trace: patched', path)
