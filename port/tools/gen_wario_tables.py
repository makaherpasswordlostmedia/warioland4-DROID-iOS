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
    handler = first function, request = Set*Pose, ECD0 = rest[0] (motion), ED00 = rest[1] (collision),
    and the last three functions of the file are ED30 = graphics/draw(u8), ED60 = music, ED90 = hitbox.
    Anything in between is a helper (ResolveWarioWater*Collision, CheckZombieWarioFloor, ...).
  The call-site type of every table is then checked against the real signature: wasm's call_indirect
  traps on a return-type or arity mismatch, so a wrong guess fails the build instead of a playthrough.

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

DEF = re.compile(r'^((?:static\s+)?(?:const\s+)?[A-Za-z_][A-Za-z0-9_\s\*]*?)\b([A-Za-z_]\w*)\(([^;{]*)\)\s*$')
SIG = {}


def source_functions(name):
    lines = (repo / 'src' / 'wario' / f'{name}.c').read_text().split('\n')
    res = []
    for i, l in enumerate(lines[:-1]):
        if lines[i + 1].strip() == '{' and not l.startswith((' ', '\t', '#')):
            m = DEF.match(l)
            if m:
                res.append(m.group(2))
                SIG[m.group(2)] = (m.group(1).strip(), m.group(3).strip())
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
    if f == 'normal':
        # normal.c continues with collision/tile helpers after the hitbox function
        pick = [rest[0], rest[1], 'DrawNormalWario', 'UpdateWarioMusicEffects', 'UpdateWarioHitbox']
        for p in pick:
            if p not in rest:
                die(f'normal: {p} missing')
    else:
        pick = [rest[0], rest[1], rest[-3], rest[-2], rest[-1]]
    # normal.c: the request slot is ApplyNormalWarioPoseTransition (it decodes 0xFE/0xFD, then calls SetNormalWarioPose);
    # the Set*Pose regex doesn't match it, so pin it explicitly.  Other reactions' slot is their Set*Pose.
    req = fn[si]
    if f == 'normal':
        if 'ApplyNormalWarioPoseTransition' not in fn:
            die('normal: ApplyNormalWarioPoseTransition missing')
        req = 'ApplyNormalWarioPoseTransition'
    names = [fn[0], req] + pick
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

# --- signature check against how the decomp calls each table ---------------------------
def argc(sig):
    a = sig[1]
    return 0 if a in ('', 'void') else len(a.split(','))

CALLSITE = {   # table -> (return type, arg count) used by the callers
    'sWarioPoseHandlerTable': ('u8', 0), 'sWarioPoseRequestFuncTable': ('void', 1),
    'sUnk_82DECD0': ('void', 0), 'sUnk_82DED00': ('void', 0), 'sUnk_82DED30': ('void', 1),
    'sUnk_82DED60': ('void', 0), 'sUnk_82DED90': ('void', 0),
}
for _, sym, _, _ in POSE_TABLES:
    CALLSITE[sym] = ('u8', 0)
problems = []
for sym, lst in list(reaction_names.items()) + list(pose_names.items()):
    want_ret, want_argc = CALLSITE[sym]
    for i, nm in enumerate(lst):
        ret, _a = SIG[nm]
        if ret != want_ret or argc(SIG[nm]) != want_argc:
            problems.append(f'{sym}[{i}] = {nm}: is {ret}({SIG[nm][1]}), callers use {want_ret}({want_argc} arg)')
if problems:
    die('signature mismatch (would trap in call_indirect):\n  ' + '\n  '.join(problems))

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
# --- block collision handlers (src/block.c) --------------------------------------------
# sBlockCollisionHandlers lives in asm/blob_0x78EBF0-0x78F5A4.s as raw ARM addresses.  Left as a
# blob, sBlockCollisionHandlers[n](&ctx) is a call_indirect on a ROM address -> wasm trap 6 the first
# time Wario touches a tile (GetWarioBlockCollisionAtPosition).  Same fix as the pose tables: map every
# address to a function name by source order and emit a real C table.
BLOCK_TABLE_ROM = 0x78F2E4
BLOCK_TABLE_LEN = 13          # block.c only indexes with tileType <= 12


def block_handlers():
    lines = (repo / 'src' / 'block.c').read_text().split('\n')
    pat = re.compile(r'^s32 (Get\w+Collision)\(struct BlockCollisionContext \*context\)$')
    names = [m.group(1) for i, l in enumerate(lines[:-1])
             if (m := pat.match(l)) and lines[i + 1].strip() == '{']
    if 'GetNonSolidBlockCollision' not in names:
        die('block.c: GetNonSolidBlockCollision not found')
    names = names[names.index('GetNonSolidBlockCollision'):]
    w = words(BLOCK_TABLE_ROM, BLOCK_TABLE_LEN)
    if any(x & 1 == 0 or not (0x08000000 <= x < 0x08800000) for x in w):
        die('sBlockCollisionHandlers: non-Thumb or out-of-ROM address (wrong ROM / wrong offset?)')
    uniq = sorted(set(w))
    if len(uniq) > len(names):
        die(f'sBlockCollisionHandlers: {len(uniq)} unique targets but only {len(names)} functions in block.c')
    lut = dict(zip(uniq, names))      # agbcc keeps source order, so ascending address == declaration order
    return [lut[x] for x in w]


# --- secondary sprite AI table (src/score.c: aiTable[id]()) ---------------------------------------
# 90 ROM addresses at 0x78F714 (blob_0x78F714-0x78F970.s).  Targets are, in ascending address == link order:
#   UpdateBigBoardSecondarySprite (hud.c), then every AI function of secondary_sprite_ai.c in source order,
#   minus the non-AI helpers below.  Verified against the ROM: spacing of consecutive targets matches the
#   function sizes (0x14 wrappers, 0x2c timed animations, 0x48 bugle notes ...).
SSAI_ROM = 0x78F714
SSAI_LEN = 90
SSAI_HELPERS = {
    'ApplySecondarySpriteVerticalMotionTable', 'ApplySecondarySpriteScaleMotionTableA',
    'ApplySecondarySpriteScaleMotionTableB', 'UpdateBugleNoteDriftMotion', 'ApplyBugleNoteRotatingAffine',
    'ApplyBugleNotePulseAffine', 'PlayAllJewelPiecesCollectedJingle', 'ApplyJewelPieceIconAffine',
    'ClampFallingSecondarySpriteAtBottom',
}


def secondary_sprite_ai():
    lines = (repo / 'src' / 'secondary_sprite_ai.c').read_text().split('\n')
    ai = []
    for i, l in enumerate(lines[:-1]):
        if lines[i + 1].strip() == '{' and not l.startswith((' ', '\t', '#')):
            m = re.match(r'^(\S+) (\w+)\((.*)\)$', l)
            if m and m.group(2) not in SSAI_HELPERS:
                ai.append(m.group(2))
    names = ['UpdateBigBoardSecondarySprite'] + ai
    w = words(SSAI_ROM, SSAI_LEN)
    if any(x & 1 == 0 or not (0x08000000 <= x < 0x08800000) for x in w):
        die('sSecondarySpriteAITable: non-Thumb or out-of-ROM address (wrong ROM / wrong offset?)')
    uniq = sorted(set(w))
    if len(uniq) > len(names):
        die(f'sSecondarySpriteAITable: {len(uniq)} unique targets but only {len(names)} candidate functions')
    lut = dict(zip(uniq, names))
    return [lut[x] for x in w]


ssai = secondary_sprite_ai()
L.append('/* secondary sprite AI: void (*)(void) */')
L.append('typedef void (*SecondarySpriteAIFn)(void);')
for nm in sorted(set(ssai)):
    L.append(f'extern void {nm}(void);')
L.append('')
L.append(f'void (*const sSecondarySpriteAITable[{SSAI_LEN}])(void) = {{')
L += [f'    {n},' for n in ssai]
L.append('};')
L.append('')

blk = block_handlers()
L.append('/* block collision handlers: s32 (*)(struct BlockCollisionContext *) */')
L.append('typedef s32 (*BlockCollisionHandlerFn)(void *);')
for nm in sorted(set(blk)):
    L.append(f'extern s32 {nm}(void *);')
L.append('')
L.append(f'const BlockCollisionHandlerFn sBlockCollisionHandlers[{BLOCK_TABLE_LEN}] = {{')
L += [f'    {n},' for n in blk]
L.append('};')
L.append('')
out.write_text('\n'.join(L))
n_slots = sum(len(v) for v in pose_names.values()) + sum(len(v) for v in reaction_names.values())
print(f'wario tables: {n_slots} slots, {len(used)} distinct functions -> {out}')
