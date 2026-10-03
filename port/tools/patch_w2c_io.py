#!/usr/bin/env python3
"""Make the wasm2c output memory-mapped-IO aware.

Supports two DEFINE_STORE macro formats across wabt versions:

  Old format (wabt < 1.0.34ish):          New format (wabt 1.0.36+):
    #define DEFINE_STORE(name, t1, t2) \\    #define DEFINE_STORE(name, t1, t2) \\
      void w2c_##name(...) {           \\      static inline void name(...) {   \\
        ...                            \\        ...                            \\
        wasm_rt_memcpy(...);           \\        wasm_rt_memcpy(...);           \\
      }                                \\      }                        ← NO backslash
      DEF_MEM_CHECKS1(...)

In both cases we insert the IO hook just before the closing `}`.

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

# A continuation line ends with backslash (then optional whitespace/newline).
CONT_RE = re.compile(r'\\\s*$')
# Closing brace WITH continuation backslash  (old format): `  }  \`
BRACE_CONT_RE = re.compile(r'^\s*\}\s*\\\s*$')
# Closing brace WITHOUT continuation backslash (new format): `  }`
BRACE_TERM_RE = re.compile(r'^\s*\}\s*$')

last_brace_idx = None
for i in range(ds_idx + 1, len(lines)):
    raw      = lines[i].rstrip('\r\n')
    stripped = raw.rstrip()
    is_cont  = bool(CONT_RE.search(raw))

    if BRACE_CONT_RE.match(stripped):
        # Old format: `}  \` is itself a continuation line; record and keep going.
        last_brace_idx = i

    if not is_cont:
        # This is the last line of the macro (no backslash).
        # New format: the `}` is the terminator itself.
        if BRACE_TERM_RE.match(stripped) and last_brace_idx is None:
            last_brace_idx = i
        break   # end of macro regardless

if last_brace_idx is None:
    sys.exit(
        'patch_w2c_io: could not locate closing } in DEFINE_STORE body – '
        'wasm2c output format unrecognised\n'
        'Lines around DEFINE_STORE:\n' +
        ''.join(lines[ds_idx:ds_idx + 15])
    )

# ── build IO-hook lines ───────────────────────────────────────────────────────
# The hook lines must be continuation lines so the macro body stays valid.
HOOK = (
    '    if ((u64)(addr - 0x04000000ull) < 0x400ull)                        \\\n'
    '      hal_io_write((unsigned)addr, (unsigned)sizeof(t1));              \\\n'
)

lines.insert(last_brace_idx, HOOK)

# ── prepend extern decl immediately above DEFINE_STORE ───────────────────────
for i, ln in enumerate(lines):
    if MACRO_DECL in ln:
        lines.insert(i, EXTERN_DECL + '\n')
        break

open(path, 'w').write(''.join(lines))
print('patch_w2c_io: patched IO hook into', path)
