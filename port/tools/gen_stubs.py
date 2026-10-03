#!/usr/bin/env python3
"""Generate weak stubs for every wasm import that the host does not implement yet.

Each stub logs once when first called, so a playthrough tells you which
asm-only functions still need a C port.

Failure modes:
  - 0 import declarations found  → regex no longer matches the header format;
    exit immediately with a clear message (don't silently write an empty file
    and let the linker fail later with confusing undefined-reference errors).
  - 0 stubs after filtering IMPL → that's fine if truly all imports are ported;
    we write a minimal valid stub file and print a notice.

usage: gen_stubs.py wl4.h out.c
"""
import re
import sys

hdr, out = sys.argv[1], sys.argv[2]
text = open(hdr).read()

# Functions that have real implementations in port/hal/ or port/rt/.
IMPL = {
    'hal_dma_set',
    'LZ77UnCompVram',
    'irq_handler',
    'hal_syscall',
    'hal_poll_vcount',
    'hal_unported_asm',
    'CPUSet',
    'CpuFastSet',
    'LZ77UnCompWram',
    'RLUnCompWram',
    'RLUnCompVram',
    'BgAffineSet',
    'ObjAffineSet',
}

# wasm2c emits a paired comment + declaration for every import:
#   /* import: 'env' 'FunctionName' */
#   rettype w2c_env_FunctionName(struct w2c_env*[, args...]);
IMPORT_RE = re.compile(
    r"/\* import: 'env' '(\w+)' \*/\n"
    r"(\w+) w2c_env_(\w+)\(struct w2c_env\*((?:,\s*\w+)*)\);"
)

matches = list(IMPORT_RE.finditer(text))

# ── hard failure: format changed, we found nothing ───────────────────────────
if not matches:
    sys.exit(
        f'gen_stubs: no import declarations found in {hdr}\n'
        '  The wasm2c header format appears to have changed.\n'
        '  Check the comment+declaration pattern and update IMPORT_RE in gen_stubs.py.\n'
        '  Expected pattern:\n'
        '    /* import: \'env\' \'FuncName\' */\n'
        '    rettype w2c_env_FuncName(struct w2c_env*[, args...]);'
    )

# ── generate stubs ───────────────────────────────────────────────────────────
preamble = [
    '#include "wl4.h"',
    '#include <stdio.h>',
    'struct w2c_env;',
]
stub_lines = []
n = 0

for m in matches:
    name, ret, _, args = m.groups()
    if name in IMPL:
        continue
    params = ''.join(
        f', {t} a{i}'
        for i, t in enumerate(a.strip() for a in args.split(',') if a.strip())
    )
    log = (
        f'static int _once_{name};'
        f' if (!_once_{name}) {{'
        f' _once_{name} = 1;'
        f' fprintf(stderr, "wl4: unimplemented import {name}\\n"); }}'
    )
    ret_stmt = ' return 0;' if ret != 'void' else ''
    stub_lines.append(
        f'__attribute__((weak)) {ret} w2c_env_{name}'
        f'(struct w2c_env *e{params}) {{ (void)e; {log}{ret_stmt} }}'
    )
    n += 1

# ── soft notice: all imports already ported ───────────────────────────────────
if n == 0:
    print(
        f'gen_stubs: notice – all {len(matches)} imports are in the IMPL set; '
        f'writing empty stub file (this is expected when the port is complete)'
    )

open(out, 'w').write('\n'.join(preamble + stub_lines) + '\n')
print(f'gen_stubs: {n} stubs -> {out}')
