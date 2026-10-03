#!/usr/bin/env python3
"""Make the wasm2c output memory-mapped-IO aware.

The game programs hardware by plain stores (e.g. `dma[2] = 0x80000000`, which
on a GBA starts a transfer immediately).  After every store whose address is
inside the IO page (0x04000000–0x040003FF) we call hal_io_write(addr, size) so
the HAL can react.

Instead of matching the exact multi-line macro text (which changes with wabt
whitespace tweaks), we parse line-by-line:

  1. Find `#define DEFINE_STORE(name, t1, t2)`.
  2. Walk the continuation lines (every line ending with a bare backslash).
  3. Locate the last line that is purely a closing brace inside that block.
  4. Insert the IO-hook lines immediately before it.
  5. Prepend the `extern` declaration once, just above DEFINE_STORE.

Idempotent: a second run on an already-patched file is a no-op.

usage: patch_w2c_io.py wl4.c
"""
import re
import sys

path = sys.argv[1]
src  = open(path).read()

MACRO_DECL  = '#define DEFINE_STORE(name, t1, t2)'
EXTERN_DECL = 'extern void hal_io_write(unsigned addr, unsigned size);'

# ── idempotency guard ────────────────────────────────────────────────────────
if '0x04000000ull' in src:
    print('patch_w2c_io: already patched,', path)
    sys.exit(0)

# ── split into lines, keeping line endings ───────────────────────────────────
lines = src.splitlines(keepends=True)

# ── find DEFINE_STORE ────────────────────────────────────────────────────────
ds_idx = next(
    (i for i, ln in enumerate(lines) if MACRO_DECL in ln),
    None
)
if ds_idx is None:
    sys.exit(
        'patch_w2c_io: DEFINE_STORE not found – '
        'wasm2c output format unrecognised (wrong file or unsupported wabt version)'
    )

# A continuation line ends with a backslash (possibly followed by spaces/CR/LF).
CONT_RE        = re.compile(r'\\\s*$')
# A closing-brace continuation line is *only* }, optional whitespace, then \.
CLOSE_BRACE_RE = re.compile(r'^\s*\}\s*\\\s*$')

# Walk the macro body to find the last `}  \` line.
last_brace_idx = None
for i in range(ds_idx + 1, len(lines)):
    stripped = lines[i].rstrip('\r\n')
    if CLOSE_BRACE_RE.match(stripped):
        last_brace_idx = i
    if not CONT_RE.search(stripped):
        break   # first non-continuation line → end of macro

if last_brace_idx is None:
    sys.exit(
        'patch_w2c_io: could not locate closing `}` in DEFINE_STORE body – '
        'wasm2c output format unrecognised'
    )

# ── build IO-hook lines (must themselves be continuation lines) ──────────────
# We use 4-space indent (standard wabt macro body indent).
HOOK = (
    '    if ((u64)(addr - 0x04000000ull) < 0x400ull)                        \\\n'
    '      hal_io_write((unsigned)addr, (unsigned)sizeof(t1));              \\\n'
)

lines.insert(last_brace_idx, HOOK)

# ── prepend extern decl immediately above DEFINE_STORE ───────────────────────
# Re-scan after the insert (index may have shifted by 1).
for i, ln in enumerate(lines):
    if MACRO_DECL in ln:
        lines.insert(i, EXTERN_DECL + '\n')
        break

open(path, 'w').write(''.join(lines))
print('patch_w2c_io: patched IO hook into', path)
