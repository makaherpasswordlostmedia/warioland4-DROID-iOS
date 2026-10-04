#!/usr/bin/env python3
"""Diagnostics: make every wasm2c function record its name in a 16-entry ring (g_wl4_trace, see host.c).
The watchdog / trap handler prints it, which names the function that contains a hang or trap.
Active only when the library is compiled with -DWL4_TRACE.  Non-fatal if FUNC_PROLOGUE is not found
(other wabt versions): tracing is simply skipped.

The hook is inserted at FILE scope right after the original FUNC_PROLOGUE/FUNC_EPILOGUE definition block
(an earlier version inserted it before the first *use*, i.e. inside a function body, so the extern
declarations were block-scoped and every other function failed to compile).

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
if not defs:
    print('patch_w2c_trace: FUNC_PROLOGUE definition not found - tracing disabled'); sys.exit(0)
for i in defs:
    lines[i] = DEF.sub(r'\1WL4_ORIG_FUNC_PROLOGUE', lines[i], count=1)
# end of the definition block: last define (+ its continuation lines), then swallow the rest of the
# preprocessor block (FUNC_EPILOGUE, #else, #endif, blank/comment lines) so we land outside any #if.
j = defs[-1]
while lines[j].rstrip().endswith('\\'): j += 1
j += 1
def skippable(l):
    s = l.strip()
    return s == '' or s.startswith('#') or s.startswith('/*') or s.startswith('//') or s.startswith('*') or lines_cont(l)
def lines_cont(l): return False
depth = 0
for k in range(defs[0]):                       # preprocessor depth at the first definition
    t = lines[k].strip()
    if re.match(r'#\s*if', t): depth += 1
    elif re.match(r'#\s*endif', t): depth -= 1
for k in range(defs[0], j):
    t = lines[k].strip()
    if re.match(r'#\s*if', t): depth += 1
    elif re.match(r'#\s*endif', t): depth -= 1
while j < len(lines) and (depth > 0 or skippable(lines[j])):
    t = lines[j].strip()
    if re.match(r'#\s*if', t): depth += 1
    elif re.match(r'#\s*endif', t): depth -= 1
    # a continued #define (backslash) swallows its following lines
    while lines[j].rstrip().endswith('\\') and j + 1 < len(lines): j += 1
    j += 1
    if depth <= 0 and j < len(lines) and not skippable(lines[j]): break
hook = (
    '#ifdef WL4_TRACE\n'
    'extern const char *volatile g_wl4_trace[16]; extern volatile unsigned g_wl4_trace_i;\n'
    '#define FUNC_PROLOGUE do { g_wl4_trace[g_wl4_trace_i++ & 15] = __func__; WL4_ORIG_FUNC_PROLOGUE; } while (0)\n'
    '#else\n'
    '#define FUNC_PROLOGUE WL4_ORIG_FUNC_PROLOGUE\n'
    '#endif\n')
lines.insert(j, hook)
open(path, 'w').write(''.join(lines))
print('patch_w2c_trace: patched', path, '(hook at line %d)' % (j + 1))
