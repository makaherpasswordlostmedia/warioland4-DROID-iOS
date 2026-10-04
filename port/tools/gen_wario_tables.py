#!/usr/bin/env python3
"""Rebuild Wario's function-pointer tables as C (wasm needs table indices, not 0x08xxxxxx).

The asm blobs hold these tables as raw ARM addresses.  This script reads them out of the
user's baserom, maps every address to a function name and emits real C tables.  Nothing
ROM-derived is committed: the ROM is read at build time and only *names* are emitted.

Mapping rule (verified, every file asserts it):
  agbcc lays functions out in source order, and linker.ld orders wario/*.o.  So within a
  file, the sorted unique addresses of a pose table are exactly the first K functions
  after the file's handler function.  Duplicated slots (several poses sharing a function)
  fall out of the address lookup, no guessing from enum names.
  The six per-reaction tables are positional:
    handler = first function, request = Set*Pose, then
    ECD0 = motion, ED00 = collision, ED30 = graphics/draw(u8), ED60 = music/anim, ED90 = hitbox
    (non-pose helpers in between, e.g. CheckZombieWarioFloor, are skipped via SKIP).

Usage: gen_wario_tables.py <repo> <baserom.gba> <out.c>
"""
import re, struct, sys
from pathlib import Path

repo, rom_path, out = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
rom = rom_path.read_bytes()

FILES = ['normal', 'swimming', 'flaming', 'fat', 'frozen', 'zombie', 'snowman',
         'bouncy', 'puffy', 'bat', 'flat', 'mask']
SKIP = {'CheckZombieWarioFloor'}

# (symbol, rom start, rom end)
REACTION_TABLES = [
    ('sWarioPoseHandlerTable', 0x2DEC70, 'WarioPoseFunc'),
    ('sWarioPoseRequestFuncTable', 0x2DECA0, 'WarioInteractionFunc'),
    ('sUnk_82DECD0', 0x2DECD0, 'WarioPoseFunc'),
    ('sUnk_82DED00', 0x2DED00, 'WarioPoseFunc'),
    ('sUnk_82DED30', 0x2DED30, 'WarioInteractionFunc'),
    ('sUnk_82DED60', 0x2DED60, 'WarioPoseFunc'),
    ('sUnk_82DED90', 0x2DED90, 'WarioPoseFunc'),
]
POSE_TABLES = [  # file index, symbol, rom start, rom end
    (0, 'sWarioNormalPoseTable', 0x2DEDC0, 0x2DEEFC),
    (1, 'sWarioWaterPoseTable', 0x2DEEFC, 0x2DEF3C),
    (2, 'sFlamingWarioPoseTable', 0x2DEF3C, 0x2DEF64),
    (3, 'sFatWarioPoseTable', 0x2DEF64, 0x2DEF90),
    (4, 'sFrozenWarioPoseTable', 0x2DEF90, 0x2DEFA0),
    (5, 'sZombieWarioPoseTable', 0x2DEFB0, 0x2DEFE4),
    (6, 'sSnowmanWarioPoseTable', 0x2DEFE4, 0x2DF024),
    (7, 'sBouncyWarioPoseTable', 0x2DF024, 0x2DF04C),
    (8, 'sPuffyWarioPoseTable', 0x2DF04C, 0x2DF05C),
    (9, 'sBatWarioPoseTable', 0x2DF05C, 0x2DF070),
    (10, 'sFlatWarioPoseTable', 0x2DF070, 0x2DF08C),
    (11, 'sWarioMaskPoseTable', 0x2DF08C, 0x2DF094),
]

DEF = re.compile(r'^(?:static\s+)?(?:const\s+)?[A-Za-z_][A-Za-z0-9_\s\*]*?\b([A-Za-z_]\w*)\([^;{]*\)\s*$')


def source_functions(name):
    lines = (repo / 'src' / 'wario' / f'{name}.c').read_text().split('\n')
    res = []
    for i, l in enumerate(lines[:-1]):
        if lines[i + 1].strip() == '{' and not l.startswith((' ', '\t', '#')):
            m = DEF.match(l)
            if m:
                res.append(m.group(1))
    return res


def words(a, n):
    return struct.unpack(f'<{n}I', rom[a:a + 4 * n])


def die(msg):
    sys.exit(f'gen_wario_tables: {msg}')


if len(rom) != 0x800000 or rom[0xAC:0xB0] != b'AWAE':
    die('baserom is not the US/EU Wario Land 4 ROM (AWAE, 8 MiB)')

funcs = {f: source_functions(f) for f in FILES}
react = {sym: words(off, len(FILES)) for sym, off, _ in REACTION_TABLES}

# --- anchor check: handler table must ascend in linker order ------------------------
h = list(react['sWarioPoseHandlerTable'])
if h != sorted(h):
    die('handler table is not ascending, linker order assumption is broken')

reaction_names = {sym: [] for sym, _, _ in REACTION_TABLES}
pose_names = {}
for idx, f in enumerate(FILES):
    fn = [x for x in funcs[f]]
    si = next(j for j, x in enumerate(fn) if re.fullmatch(r'Set\w*Pose', x))
    rest = [x for x in fn[si + 1:] if x not in SKIP]
    if len(rest) < 5:
        die(f'{f}: expected >=5 functions after {fn[si]}, got {rest}')
    # normal: movement, collision, ..., draw, music, hitbox (last three of the reaction-visible set)
    if f == 'normal':
        pick = [rest[0], rest[1], 'DrawNormalWario', 'UpdateWarioMusicEffects', 'UpdateWarioHitbox']
        for p in pick:
            if p not in rest:
                die(f'normal: {p} missing')
    else:
        pick = rest[:5]
    names = [fn[0], fn[si]] + pick
    for (sym, _, _), nm in zip(REACTION_TABLES, names):
        reaction_names[sym].append(nm)
    # ascending-address sanity: the seven addresses of this reaction must ascend in this order
    addrs = [react[sym][idx] for sym, _, _ in REACTION_TABLES]
    if addrs != sorted(addrs):
        die(f'{f}: per-reaction addresses are not ascending: {[hex(a) for a in addrs]}')

for idx, sym, a, b in POSE_TABLES:
    f = FILES[idx]
    w = words(a, (b - a) // 4)
    uniq = sorted(set(w))
    fn = funcs[f]
    si = next(j for j, x in enumerate(fn) if re.fullmatch(r'Set\w*Pose', x))
    k = len(uniq)
    cand = fn[1:1 + k]
    if 1 + k > si:
        die(f'{sym}: {k} unique targets do not fit before {fn[si]} ({si - 1} candidates)')
    handler = react['sWarioPoseHandlerTable'][idx]
    if uniq[0] <= handler or uniq[-1] >= react['sWarioPoseRequestFuncTable'][idx]:
        die(f'{sym}: targets fall outside [{hex(handler)}, {hex(react["sWarioPoseRequestFuncTable"][idx])})')
    if any(x & 1 == 0 or not (0x08000000 <= x < 0x08800000) for x in w):
        die(f'{sym}: non-Thumb or out-of-ROM address')
    lut = dict(zip(uniq, cand))
    pose_names[sym] = [lut[x] for x in w]

# --- emit ------------------------------------------------------------------------------
used = set()
for lst in list(reaction_names.values()) + list(pose_names.values()):
    used.update(lst)

L = ['/* generated by port/tools/gen_wario_tables.py - names only, no ROM data */',
     '/* wario.h is deliberately not included: the decomp enums are shorter than the ROM tables',
     '   (WPOSE_NORMAL_COUNT is 76, the ROM table has 79 slots) and its extern sizes would conflict. */',
     '#include "types.h"',
     'typedef u8 (*WarioPoseFunc)(void);',
     'typedef void (*WarioInteractionFunc)(u8);',
     f'#define REACTION_COUNT {len(FILES)}', '']
for nm in sorted(used):
    L.append(f'extern void {nm}();')
L.append('')
tmap = {'WarioPoseFunc': 'WarioPoseFunc', 'WarioInteractionFunc': 'WarioInteractionFunc'}
for sym, _, ty in REACTION_TABLES:
    decl = f'{ty} {sym}[REACTION_COUNT]'
    L.append(decl + ' = {')
    L += [f'    (void *){n},' for n in reaction_names[sym]]
    L.append('};')
    L.append('')
for idx, sym, a, b in POSE_TABLES:
    L.append(f'WarioPoseFunc {sym}[{(b - a) // 4}] = {{')
    L += [f'    (void *){n},' for n in pose_names[sym]]
    L.append('};')
    L.append('')
out.write_text('\n'.join(L))
n_slots = sum(len(v) for v in pose_names.values()) + sum(len(v) for v in reaction_names.values())
print(f'wario tables: {n_slots} slots, {len(used)} distinct functions -> {out}')
