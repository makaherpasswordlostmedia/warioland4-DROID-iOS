#!/usr/bin/env python3
"""Copy decomp sources into a build tree and make them compile for wasm32.

The decomp targets agbcc/ARM7TDMI and contains matching hacks that are not valid
for a normal compiler.  This script rewrites a *copy* of src/ and include/:

  * `register T x asm("rN")`   -> `T x`      (register pinning)
  * empty `asm("" : "+r"(x))`  -> removed    (optimizer barriers)
  * simple one-instruction Thumb inline asm (mov/add/ldr/str/...) -> plain C
  * anything else is left untouched and listed in <out>/UNTRANSLATED.txt

The original tree is never modified.
"""
import re, shutil, sys
from collections import Counter
from pathlib import Path

root = Path(sys.argv[1]); out = Path(sys.argv[2])

REG_NAMES = r'(?:r\d+|ip|sl|fp|sb|sp|lr)'
ASM_REG = re.compile(r'\s*\basm\("' + REG_NAMES + r'"\)')
REGISTER = re.compile(r'\bregister\s+')
WS = r'(?:\s|\\\n)*'          # whitespace incl. macro line continuations
ASM_STMT = re.compile(
    r'\basm' + WS + r'(?:volatile|__volatile__)?' + WS + r'\(' + WS + r'((?:"(?:[^"\\]|\\.)*"' + WS + r')+)((?::[^;]*?)?)\)' + WS + r';', re.S)
STR = re.compile(r'"((?:[^"\\]|\\.)*)"')

import os
STRICT = os.environ.get('PORT_STRICT') == '1'
SITE_IDS = []
MACRO_NOOPS = []
TIED_ASM = []
AUDIT = []
untranslated = Counter()
CUR = ['', '']

# ---- tiny Thumb -> C translator ------------------------------------------
def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '[': depth += 1
        if ch == ']': depth -= 1
        if ch == ',' and depth == 0: out.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

class Fail(Exception): pass

def parse_operands(constraints):
    """C expressions for %0.. in order (outputs, then inputs)."""
    parts = re.split(r'(?<!:):(?!:)', constraints)
    ops = []
    for sec in parts[1:3]:
        for m in re.finditer(r'"([^"]*)"\s*\(([^()]*(?:\([^()]*\))?[^()]*)\)', sec):
            ops.append((m.group(1), m.group(2).strip()))
    return ops

REGMAP = {}          # literal register -> C variable (pinned declarations in scope)
ALIAS = {'sb': 'r9', 'sl': 'r10', 'fp': 'r11', 'ip': 'r12'}
REGRE = re.compile(r'r(?:\d|1[0-2])')

class Ctx:
    """Per-template state: literal registers that are not pinned variables become
    block-local temporaries.  Reading one before it was written is a failure
    (it would depend on whatever agbcc left in that register)."""
    def __init__(self, ops): self.ops, self.temps, self.written = ops, set(), set()
    def val(self, a, write=False):
        a = a.strip()
        if a.startswith('#'): return a[1:]
        m = re.fullmatch(r'%(\d+)', a)
        if m: return self.ops[int(m.group(1))][1]
        a = ALIAS.get(a, a)
        if a in REGMAP:
            # With %-operands present, trust a pinned declaration only if that variable is itself one of
            # the asm operands; otherwise the allocator decides what the register holds (e.g. catbat.c `add r2, r0, r2 : "+r"(sprites) : "r"(offset)`).
            if self.ops and REGMAP[a] not in [o[1] for o in self.ops]: raise Fail(a)
            return REGMAP[a]
        if REGRE.fullmatch(a):
            if write: self.written.add(a)
            elif a not in self.written: raise Fail(a)
            self.temps.add(a); return '_t' + a
        raise Fail(a)
    def mem(self, a):
        m = re.fullmatch(r'\[\s*([^,\]]+)\s*(?:,\s*([^\]]+))?\]', a.strip())
        if not m: raise Fail(a)
        base = self.val(m.group(1))
        off = '0' if m.group(2) is None else self.val(m.group(2))
        return f'((u8*)((u32)({base}) + (u32)({off})))'

LD = {'ldr': 'u32', 'ldrh': 'u16', 'ldrb': 'u8', 'ldrsh': 's16', 'ldrsb': 's8'}
ST = {'str': 'u32', 'strh': 'u16', 'strb': 'u8'}
ALU = {'add': '+', 'adds': '+', 'sub': '-', 'subs': '-', 'orr': '|', 'orrs': '|',
       'and': '&', 'ands': '&', 'eor': '^', 'eors': '^', 'mul': '*', 'muls': '*',
       'lsl': '<<', 'lsls': '<<', 'lsr': '>>', 'lsrs': '>>', 'asr': 'ASR', 'asrs': 'ASR'}

def assign(d, rhs): return f'{d} = (__typeof__({d}))({rhs});'

def translate(template, constraints):
    ctx = Ctx(parse_operands(constraints))
    lines = [l.strip() for l in re.split(r'\\n|\\t|\n|;', template) if l.strip()]
    stmts = []
    try:
        for line in lines:
            m = re.match(r'(\w+)\s*(.*)', line)
            if not m: return None
            op, rest = m.group(1), split_args(m.group(2))
            if op in ('mov', 'movs') and len(rest) == 2:
                s_ = ctx.val(rest[1]); stmts.append(assign(ctx.val(rest[0], True), f'(u32)({s_})'))
            elif op in ('neg', 'mvn', 'negs') and len(rest) == 2:
                s_ = ctx.val(rest[1]); stmts.append(assign(ctx.val(rest[0], True), ('-(s32)' if op != 'mvn' else '~(u32)') + f'({s_})'))
            elif op in ('sxth', 'uxth', 'sxtb', 'uxtb') and len(rest) == 2:
                t = {'sxth': 's16', 'uxth': 'u16', 'sxtb': 's8', 'uxtb': 'u8'}[op]
                s_ = ctx.val(rest[1]); stmts.append(assign(ctx.val(rest[0], True), f'({t})({s_})'))
            elif op in ALU and len(rest) in (2, 3):
                a = ctx.val(rest[1] if len(rest) == 3 else rest[0]); b = ctx.val(rest[2] if len(rest) == 3 else rest[1])
                d = ctx.val(rest[0], True)
                sym = ALU[op]
                if sym == 'ASR': expr = f'(s32)({a}) >> ({b})'
                elif sym in ('<<', '>>'): expr = f'(u32)({a}) {sym} ({b})'
                else: expr = f'(u32)({a}) {sym} (u32)({b})'
                stmts.append(assign(d, expr))
            elif op in LD and len(rest) == 2:
                addr = ctx.mem(rest[1]); stmts.append(assign(ctx.val(rest[0], True), f'*({LD[op]}*){addr}'))
            elif op in ST and len(rest) == 2:
                s_ = ctx.val(rest[0]); addr = ctx.mem(rest[1]); stmts.append(f'*({ST[op]}*){addr} = ({ST[op]})({s_});')
            else:
                return None
    except Fail:
        return None
    decl = ''.join(f'u32 _t{r}; ' for r in sorted(ctx.temps))
    return '{ ' + decl + ' '.join(stmts) + ' }'

BR = {'beq': '==', 'bne': '!=', 'blt': '<', 'bge': '>=', 'ble': '<=', 'bgt': '>'}
TARGET = r'(1f|\.\w+)'
CMPBR = re.compile(r'\s*cmp\s+(%\d+)\s*,\s*#(-?\w+)\s*(?:\\n|\\t|\n|;)\s*(beq|bne|blt|bge|ble|bgt)\s+' + TARGET + r'\s*$')
JMP = re.compile(r'\s*b\s+' + TARGET + r'\s*$')
LABEL1 = re.compile(r'\s*(1|\.\w+):\s*$')

def goto_tok(t): return '@@GOTO1@@' if t == '1f' else 'goto _asm' + t.replace('.', '_') + ';'


# ---- hand-ported inline asm (wario/normal.c) ------------------------------
# agbcc needed these asm "call boundaries" only to keep a byte-matching stack frame.
# Under clang the plain C call is equivalent, so each site is replaced by the C it encodes.
# Stack layout the asm relied on: [sp+0]=5th arg, then the locals struct: sp+4=adjustedX, sp+6=adjustedY, sp+8=secondary.
# Keyed by (file, line of the `asm` keyword in the ORIGINAL source); the template must contain the
# given marker, otherwise portify aborts (source moved -> re-check by hand).
_CALL = 'CheckWarioTileCollision'
_RD = lambda var, fld: f'{var} = locals.{fld};'
ASM_OVERRIDES = {
  ('src/wario/normal.c', 5662): ('ldrsh %0, [%0, r4]',  'pointAddress = (u32)(s32)*(s16 *)pointAddress;'),
  ('src/wario/normal.c', 6262): ('ldrh r0, [r1, #8]',   _RD('secondaryValue', 'secondary')),
  ('src/wario/normal.c', 6277): ('ldrh r0, [r2, #6]',   _RD('storedY', 'adjustedY')),
  ('src/wario/normal.c', 6330): ('add r6, sp, #8',      'secondaryOut = &locals.secondary;'),
  ('src/wario/normal.c', 6333): ('bl CheckWarioT',      f'(void){_CALL}(leftIndex, yPosition, &locals.adjustedX, &locals.adjustedY, secondaryOut);'),
  ('src/wario/normal.c', 6368): ('bl CheckWarioT',      f'callResult = {_CALL}(leftIndex, yPosition, &locals.adjustedX, &locals.adjustedY, secondaryOut);'),
  ('src/wario/normal.c', 6387): ('ldrh r0, [r2, #6]',   _RD('storedY', 'adjustedY')),
  ('src/wario/normal.c', 6414): ('bl CheckWarioT',      f'(void){_CALL}(leftIndex, yPosition, &locals.adjustedX, &locals.adjustedY, secondaryOut);'),
  ('src/wario/normal.c', 6436): ('ldrh r0, [r4, #18]',  f'(void){_CALL}(currentWario->xPosition, yPosition, &locals.adjustedX, &locals.adjustedY, &locals.secondary);'),
  ('src/wario/normal.c', 6450): ('ldrh r0, [r2, #8]',   _RD('secondaryValue', 'secondary')),
  ('src/wario/normal.c', 6529): ('ldrh r2, [r6, #20]',  'yPosition = wario->yPosition;'),
  ('src/wario/normal.c', 6540): ('add r3, r5, #0',      f'callResult = {_CALL}(xPosition, yPosition, &locals.adjustedX, adjustedYPtr, secondaryPtr);'),
  ('src/wario/normal.c', 6576): ('add r3, r5, #0',      f'callResult = {_CALL}(xPosition, yPosition, &locals.adjustedX, adjustedYPtr, secondaryPtr);'),
  ('src/wario/normal.c', 6641): ('ldrh r2, [r6, #20]',  'yPosition = wario->yPosition;'),
  ('src/wario/normal.c', 6652): ('add r3, r4, #0',      f'callResult = {_CALL}(xPosition, yPosition, &locals.adjustedX, adjustedYPtr, secondaryPtr);'),
  ('src/wario/normal.c', 6668): ('ldrh r0, [r1, #6]',   _RD('adjustedY', 'adjustedY')),
  ('src/wario/normal.c', 6693): ('bl CheckWarioT',      f'callResult = {_CALL}(xPosition, yPosition, &locals.adjustedX, &locals.adjustedY, secondaryPtr);'),
  ('src/wario/normal.c', 6710): ('ldrh r0, [r2, #6]',   _RD('adjustedY', 'adjustedY')),
  ('src/wario/normal.c', 6764): ('ldrh r0, [r2, #6]',   _RD('adjustedY', 'adjustedY')),
  ('src/wario/normal.c', 7215): ('ldrh r2, [r2, #6]',   _RD('adjustedY', 'adjustedY')),
  # r5 = r8 = previousY (asm outputs), call(firstX, previousY, ...), result back in previousY
  ('src/wario/normal.c', 7223): ('mov r8, r5',
      '{ u32 _py = previousY; adjustedYPtr = (u16 *)_py; firstY = _py; '
      f'previousY = {_CALL}(firstX, _py, &locals.adjustedX, &locals.adjustedY, &locals.secondary); }}'),
  ('src/wario/normal.c', 7287): ('str r4, [sp, #0]',   f'callResult = {_CALL}(firstX, firstY, &locals.adjustedX, &locals.adjustedY, (u16 *)r4Value);'),
  ('src/wario/normal.c', 7367): ('add r2, sp, #8',      f'callX = {_CALL}(callX, callY, &locals.adjustedX, &locals.adjustedY, &locals.secondary);'),
  ('src/wario/normal.c', 7502): ('ldrsh r0, [r0, r4]',  'tableValue = (u32)(s32)*(s16 *)tableValue; r4Value = 0;'),
  ('src/wario/normal.c', 7515): ('add r0, sp, #8',      f'callResult = {_CALL}(alternateX, alternateY, &locals.adjustedX, &locals.adjustedY, &locals.secondary);'),
}

# ---- hand-ported inline asm (sprite_ai/catbat.c, sprite_ai/swinging_platform.c) ---------------
# Every site is one of: (a) a commutative `add` that agbcc emitted with swapped operands (the result is
# just dst = a + b), (b) a signed-halfword table load whose zero index was pinned to a register, or
# (c) a tiny branch/literal trick.  The register names inside the templates refer to the pinned
# `register T x asm("rN")` locals of the enclosing block, which is how each one was mapped to a variable.
_ADD = lambda d, a, b: f'{d} = (__typeof__({d}))((u32)({a}) + (u32)({b}));'
_CB, _SP = 'src/sprite_ai/catbat.c', 'src/sprite_ai/swinging_platform.c'
ASM_OVERRIDES.update({
  # catbat: dst += table base
  (_CB, 133):  ('add r0, r0, r2', _ADD('offset', 'offset', 'tableBase')),
  (_CB, 177):  ('add r0, r0, r2', _ADD('offset', 'offset', 'table')),
  (_CB, 192):  ('add r0, r0, r2', _ADD('offset', 'offset', 'tableBase')),
  (_CB, 414):  ('add r0, r0, r5', _ADD('entryAddress', 'entryAddress', 'table')),
  (_CB, 480):  ('add r0, r0, r5', _ADD('entryAddress', 'entryAddress', 'table')),
  (_CB, 532):  ('add r0, r0, r5', _ADD('entryAddress', 'entryAddress', 'table')),
  (_CB, 581):  ('add r0, r0, r5', _ADD('entryAddress', 'entryAddress', 'table')),
  (_CB, 623):  ('add r0, r0, r5', _ADD('entryAddress', 'entryAddress', 'table')),
  (_CB, 729):  ('add r7, r7, r4', _ADD('indexPtr', 'indexPtr', 'sprite')),      # work3 pointer: (u8 *)42 + sprite
  (_CB, 2175): ('add r2, r0, r2', _ADD('sprites', 'sprites', 'offset')),          # &gSpriteData[slot]
  (_CB, 2453): ('add r2, r0, r1', _ADD('slot', 'offset', 'base')),                # &gSpriteData[slot] (kept as integer)
  (_CB, 2687): ('add r0, r0, r6', _ADD('entryAddress', 'entryAddress', 'velocityTable')),
  # catbat: ldr r0,[pc,#44] re-reads the literal that the store above it used: &gBossDefeatTimer
  # (inferred from the agbcc comment and the store; not checked against ROM bytes)
  (_CB, 448):  ('ldr r0, [pc, #44]', 'switchRead = (vu8 *)gBossDefeatTimer;'),
  # catbat: r7 is set to 0 and copied to zeroHigh (r7 itself is dead afterwards)
  (_CB, 753):  ('mov r7, #0', 'zeroHigh = 0;'),
  # catbat: (8 & timer) normalized through a byte round trip; timer is a 0..255 int here
  (_CB, 1300): ('and r0, r0, %1', 'normalizedBit = (int)(((u32)(8 & timer) << 24) >> 24);'),
  # catbat: asm skips `sprite->health |= healthBit` when (roomSlot & roomBit) == 0.
  # r4 = sprite (&gCurrentSprite), [r4,#24] = roomSlot, r5 = roomBit (== 1) in InitCatbatLandingDebris.
  # The matching `asm volatile("1:")` label that follows is handled by the generic label logic.
  (_CB, 2188): ('ldrb r1, [r4, #24]', 'if ((sprite->roomSlot & roomBit) == 0) @@GOTO1@@;'),
})
_LDX = lambda d, a: f'{d} = (s32)*(s16 *)(u32)({a});'
for _ln, _tbl in ((216, 'xTable'), (731, 'xTable')):
    ASM_OVERRIDES[(_SP, _ln)] = ('add r0, r3, r1', f'xVelocity = (s32)*(s16 *)((u32)(frameOffset) + (u32)({_tbl}));')
for _ln in (307, 822):
    ASM_OVERRIDES[(_SP, _ln)] = ('add r0, r3, r4', 'xVelocity = (s32)*(s16 *)((u32)(frameOffset) + (u32)(xTable));')
for _ln in (287, 802):
    ASM_OVERRIDES[(_SP, _ln)] = ('ldrsh r0, [r0, r3]', _LDX('xVelocity', 'xPointer'))
for _ln in (534, 1049):
    # r1 += r0 (address), r0 = *(s16 *)r1 * 4;  r0 is `table`, r1 is `byteOffset`; both are outputs
    ASM_OVERRIDES[(_SP, _ln)] = ('add r1, r1, r0',
        '{ u32 _a = (u32)byteOffset + (u32)table; byteOffset = (int)_a; '
        'table = (const s16 *)(u32)((s32)*(s16 *)_a * 4); }')
for _ln in (546, 1061):
    ASM_OVERRIDES[(_SP, _ln)] = ('add r0, r5, r0', 'lowAngleTable = (const u8 *)((u32)frame + (u32)lowAngleTable); angle = *lowAngleTable;')
for _ln in (560, 1075):
    ASM_OVERRIDES[(_SP, _ln)] = ('ldrsh r2, [r0, r3]', _LDX('value', 'index'))
for _ln in (573, 1088):
    ASM_OVERRIDES[(_SP, _ln)] = ('ldrsh r4, [r0, r1]', _LDX('sine', 'index'))

# ---- hand-ported inline asm (minigames/roulette/*) --------------------------------------------
# draw_roulette.c: the asm only pins registers or reorders operands.  Mapping used (checked against the pinned
# `register ... asm("rN")` locals of each enclosing block):
#   `mov %0, sp ; add %0,%0,#2/4/6`   -> &stack.affineB/C/D   (RouletteRenderStack lives at sp+0; affineBPtr is at sp+8, affineCPtr at sp+12)
#   `mov %0, sp`                       -> &stack.affineA
#   `mov r2, %1 ; and %0, r2`          -> coord &= xMask
#   `mov rX, r8 ; ... ldrsh r0,[..,#4]` -> mainState->scale (r8 = mainState in the first part of DrawRoulette)
#   `... ldrsh r0,[r0,#4]` on a RouletteItem * -> item->scale
#   `ldr r2,[sp,#8/#12] ; strh r0,[r2]` -> *stack.affineBPtr / *stack.affineCPtr = (u16)result
#   `mov %1, r8` in the OAM priority code -> nibbleMask (r8 = 15 there)
#   `mov %0, sl` / `mov rX,#13 ; neg ; mov %0,rX` -> -13 (sl is only ever set to -13; the two `mov sl, r4` setters become no-ops)
# Three sites were NOT reported before, because the generic translator "succeeded" on them while dropping the result
# (it wrote to a block-local temporary instead of the %0 operand, or clobbered `oam`): lines 150, 831, 935.
_RL = 'src/minigames/roulette/draw_roulette.c'
_RL_DRAW = [
  (57, 'mov %0, sp', 'affineBAddress = &stack.affineB;'),
  (60, 'mov %0, sp', 'affineCAddress = &stack.affineC;'),
  (63, 'mov %0, sp', 'affineDAddress = &stack.affineD;'),
  (97, 'and %0, r2', 'coord &= xMask;'),
  (150, 'ldrsh', 'sin64 = (s32)*(s16 *)(u32)(sinPtr);'),
  (152, 'ldrsh r0', 'scaleValue = (s32)*(s16 *)((u8 *)mainState + 4);'),
  (164, 'ldrsh', 'sin0 = (s32)*(s16 *)(u32)(sinPtr);'),
  (166, 'ldrsh r0', 'scaleValue = (s32)*(s16 *)((u8 *)mainState + 4);'),
  (182, 'ldrsh r0', 'scaleValue = (s32)*(s16 *)((u8 *)mainState + 4);'),
  (193, 'ldrsh r0', 'scaleValue = (s32)*(s16 *)((u8 *)mainState + 4);'),
  (279, 'and %0, r2', 'coord &= xMask;'),
  (372, 'mov %0, sp', 'affineCAddress = &stack.affineC;'),
  (484, 'mov %1, r8', 'low = priority; maskTemp = nibbleMask;'),
  (503, 'mov %1, r8', 'low = priority; maskTemp = nibbleMask;'),
  (531, 'ldrsh', 'sin64 = (s32)*(s16 *)(u32)(sinPtr);'),
  (540, 'ldrsh r0', 'scaleItem = (__typeof__(scaleItem))(s32)*(s16 *)((u8 *)scaleItem + 4);'),
  (551, 'mov r8, r2', 'sin0 = (s32)*(s16 *)(u32)(zeroSinPtr);'),
  (557, 'ldrsh r0', 'scaleItem = (__typeof__(scaleItem))(s32)*(s16 *)((u8 *)scaleItem + 4);'),
  (565, 'ldr r2, [sp', '*stack.affineBPtr = (u16)result;'),
  (582, 'ldrsh r0', 'scaleItem = (__typeof__(scaleItem))(s32)*(s16 *)((u8 *)scaleItem + 4);'),
  (590, 'ldr r2, [sp', '*stack.affineCPtr = (u16)result;'),
  (597, 'ldrsh r0', 'scaleItem = (__typeof__(scaleItem))(s32)*(s16 *)((u8 *)scaleItem + 4);'),
  (607, 'ldrsh %0', 'scaleCheck = (s32)*(s16 *)((u8 *)scaleItem + 4);'),
  (671, 'mov %0, sp', 'firstValue = (int)(u32)&stack.affineA;'),
  (757, 'mov %0, sl', 'out = -13;'),
  (772, 'and %0, r2', 'coord &= xMask;'),
  (791, 'mov %0, sl', 'out = -13;'),
  (831, 'mov sl, r4', ';'),
  (858, 'mov %0, sl', 'out = -13;'),
  (861, 'orr %0, r2', 'out |= 4;'),
  (876, 'and %0, r2', 'coord &= xMask;'),
  (895, 'mov %0, sl', 'out = -13;'),
  (935, 'mov sl, r4', ';'),
  (962, 'mov %0, sl', 'out = -13;'),
  (965, 'orr %0, r2', 'out |= 4;'),
  (980, 'and %0, r2', 'coord &= xMask;'),
  (999, 'mov %0, sl', 'out = -13;'),
  (1090, 'neg', 'attrByteMask = -13;'),
  (1115, 'and %0, r2', 'coord &= xMask;'),
  (1207, 'neg', 'attrByteMask = -13;'),
  (1232, 'and %0, r2', 'coord &= xMask;'),
  (1317, 'and %0, r2', 'coord &= xMask;'),
]
for _ln, _mk, _rep in _RL_DRAW: ASM_OVERRIDES[(_RL, _ln)] = (_mk, _rep)
_RLD = 'src/minigames/roulette/'
ASM_OVERRIDES.update({
  # init_roulette_game.c: same loop as reset_roulette_round.c (`dst[i] = MinigameRandom() % gRouletteValueCount`).  The asm
  # only kept agbcc's register layout; `ldr r1,[pc,#172]; ldrh r1,[r1]` is gRouletteValueCount (inferred from that twin loop).
  (_RLD + 'init_roulette_game.c', 458): ('bl MinigameRandom',
      'gRouletteWinningValues[randomIndex] = (u8)__modsi3(MinigameRandom(), gRouletteValueCount);'),
  # update_roulette_wheel.c: acceleration += speed (both u16), result also returned in `sum`
  (_RLD + 'update_roulette_wheel.c', 214): ('ldrh r0, [r0]',
      'sum = (u16)(*(u16 *)speedPtr2 + *(u16 *)accumulatorPtr); *(u16 *)accumulatorPtr = sum;'),
  # r2 = &gRouletteAngularAcceleration (left over from the block above, per the source comment), r4 = zero
  (_RLD + 'update_roulette_wheel.c', 231): ('strh r4, [r2]', 'gRouletteAngularAcceleration = zero;'),
})


# ---- hand-ported inline asm (minigames/wario_hop.c) -------------------------------------------
# 16 of the 29 sites are `.macro ... .endm` register-steering and are handled by _macro_only().  The other 13:
#   WarioHopInit: pinned-register byte/half stores (r5/r4 hold two equivalent zeros), a 12-byte ldmia/stmia
#   struct copy, a 5-store loop body, and two `ldrb r*,[r7]` reloads of gWarioHopJumpDuration (r7 = speedPtr).
#   WarioHopUpdateGameplay: `.4byte 0xfa46f000` is a Thumb BL pair -> +0x48C from the call site.  The only unreferenced
#     s32(void) function in the file is WarioHopCheckPlayerCollision; summing the progress.html sizes of the functions
#     in between leaves a 56-byte tail for the rest of UpdateGameplay, which fits.
#   WarioHopUpdateJumpArc: `mov r1,#2; ldrsh %0,[%1,r1]` = first->y as s16.
#   WarioHopDrawSprites: item animation advance (ldrh/lsl/add/ldrb/cmp + `bcs`), and `ldr %0,[sp,#16]` = affineDPtr
#     (frame: A@0 B@2 C@4 D@6 pad BPtr@8 CPtr@12 DPtr@16, same layout as roulette).
_WH = 'src/minigames/wario_hop.c'
ASM_OVERRIDES.update({
  (_WH, 336):  ('ldrb r0, [r7]',   'speed = (s32)*(u8 *)speedPtr;'),
  (_WH, 342):  ('ldrb r1, [r7]',   'speed2 = (s32)*(u8 *)speedPtr;'),
  # r0 = gWarioHopRollingObject (just stored [0],[1]); +6/+8 = animTimer/animFrame of a WarioHopSmallState-shaped object
  (_WH, 383):  ('strh r4, [r0, #6]', 'gWarioHopRollingObject[3] = (u16)zero; gWarioHopRollingObject[4] = (u16)zero;'),
  (_WH, 396):  ('strb r5, [r2]',   '*subStatePtr = (u8)zero;'),
  (_WH, 400):  ('strb r5, [r6]',   '*highScorePtr = (u8)zero;'),
  (_WH, 407):  ('strb r5, [r0]',   '*medalTimerPtr = (u8)zero;'),
  # x=0, type=0, animTimer=0, animFrame=0, active=1  (struct WarioHopSmallState; y at +2 is left alone)
  (_WH, 427):  ('strh r2, [r0]',   'item->x = 0; item->type = 0; item->animTimer = 0; item->animFrame = 0; item->active = (u8)one;'),
  (_WH, 477):  ('ldmia r0!, {r2, r4, r7}', '*medalEffect = *bonusEffect;'),
  (_WH, 591):  ('.4byte 0xfa46f000', 'result = WarioHopCheckPlayerCollision();'),
  (_WH, 653):  ('mov r1, #2',      'frame = (s32)*(s16 *)((u8 *)first + 2);'),
  (_WH, 1568): ('ldrh %0, [%2, #8]', 'frameTime = currentAnim[currentItem->animFrame].time; timer = (u16)timer;'),
  (_WH, 1581): ('bcs .LwarioHopItemAnimNoAdvance', 'if (frameTime >= timer) goto _asm_LwarioHopItemAnimNoAdvance;'),
  (_WH, 1999): ('ldr %0, [sp, #16]', 'affineDst = affineDPtr;'),
})

# ---- hand-ported inline asm (minigames/homerun_derby.c) ---------------------------------------
# Thumb halfwords in `.4byte`/`.word` were decoded by hand (little-endian: low halfword is the first instruction).
#   404   0xfbb2f7fb = BL -0x489C, r0 = 120 in/out.  Site is ~0x464 into UpdateHomerunDerby (S+0x464); target S-0x4434 is
#         MinigameWaitForFrames(s32) (sizes from docs/progress.html, linker order).  Only function there that takes r0 and
#         returns "done?" (MinigameRandom at S-0x4450 takes no arg).  Inferred, not checked against ROM bytes.
#   442   0xfa8ef001 = BL +0x151C, no args, result in r0.  Site ~S+0x4F0; target S+0x1A10 = UpdateHomerunBonusAnimation, which has
#         no other caller in the file.  Inferred.
#   634   `mov r7,%0; str r7,[r1]` with r1 = DMA3 base of this block  ->  DMA3->src = backgroundTilemapD
#   673   `strb r1,[r7]` in the else branch (difficulty != 2).  r1 is zeroValue (still live from the BG-scroll block), so the
#         initial pitch level is 0 outside hard mode (the if-branch stores 1; level counts 0..9).  Inferred.
#   721   0x3952 = sub r1,#0x52 (r1 held 0x04000052 from the store above -> 0x04000000), 0x2780 = movs r7,#0x80
#   930/932  `ldr r0,=0x233; bl m4aSongNumStart` and `mov r0,#1; b <over pool>; .4byte 0x233`: normal-hit sound, then return 1
#         (the branch skips the trailing `return 0`).  Sound id is read from the literal in the source itself.
#   1675  `ldr r2,[pc,#356]; mov ip,r2`: pool literal, not in the source.  ip is the attr1 preserve-mask (same role as sl in the
#         bonus loop below: 0xFFFFFE00, x lives in bits 0..8).  Inferred.
#   1747  0x00c0 = lsl r0,r0,#3 ; 0x1900 = add r0,r0,r4   (frame index * 8 + animation base)
#   1788  0x00f8 = lsl r0,r7,#3 ; 0x1844 = add r4,r0,r1   (firstOamIndex*8 + gOamBuffer in r1)
#   1797  0x3302 = add r3,#2 ; 0x8032 = strh r2,[r6]      (frameData++, *dest = attr)
#   1841..1844  m4aSongNumStart(0x229) then `b` (0xe00b) to the end of the switch == break
_HD = 'src/minigames/homerun_derby.c'
ASM_OVERRIDES.update({
  (_HD, 404):  ('.4byte 0xfbb2f7fb', 'nextValue = MinigameWaitForFrames(120);'),
  (_HD, 442):  ('.4byte 0xfa8ef001', 'nextValue = UpdateHomerunBonusAnimation();'),
  (_HD, 634):  ('str r7, [r1]',      'dmaRegisters->src = (u32)backgroundTilemapD;'),
  (_HD, 673):  ('strb r1, [r7]',     '*pitchLevelPointer = (u8)zeroValue;'),
  (_HD, 721):  ('.4byte 0x27803952', 'displayRegister = (vu16 *)0x04000000; displayControl = 0x80;'),
  (_HD, 930):  ('.LhomerunSound233', 'm4aSongNumStart(0x233U);'),
  (_HD, 932):  ('mov r0, #1',        'return 1;'),
  (_HD, 1675): ('ldr r2, [pc, #356]', 'oamAttributeMask = (s32)0xFFFFFE00;'),
  (_HD, 1747): ('.word 0x190000c0',  'bonusFrame = (struct AnimationFrame *)(((u32)bonusFrame << 3) + (u32)bonusAnimationFrames);'),
  (_HD, 1788): ('.word 0x184400f8',  'bonusOamBytes = (OamByte *)(((u32)firstOamIndex << 3) + (u32)positionValue);'),
  (_HD, 1797): ('.word 0x80323302',  'frameData++; *(u16 *)oamDataDestination = (u16)oamAttribute2;'),
  (_HD, 1841): ('ldr r0, [pc, #8]',  'm4aSongNumStart(0x229U); break;'),
  (_HD, 1842): ('bl m4aSongNumStart', ';'),
  (_HD, 1843): ('.word 0x0000e00b',  ';'),
  (_HD, 1844): ('.word 0x00000229',  ';'),
})

# ---- hand-ported inline asm (sprite_ai/cractus.c) ----------------------------------------------
# All five are agbcc register-allocation steering; the semantics follow from the source and struct offsets
# (PrimarySpriteData: xPosition=0x0A, work2=0x29, work3=0x2A), no ROM bytes needed.
#   1947  `lsl r0,r0,#24; lsr %0,r0,#24` = (u8) of the SpriteUtilFindSprite(219,0) return, into slot219.  The bare call on the
#         line above is folded into this assignment by a PATCHES rule (otherwise the call would run twice).
#   2014  r1 = &current->work2; count = *r1 - 1; *r1 = (u8)count   (count keeps the 32-bit result, masked right after)
#   2370  current->xPosition -= 4
#   3646/3668  r3 = &gCurrentSprite (not an operand); `strb r2,[r3+42]` = gCurrentSprite.work3 = (u8)yDelta
_CR = 'src/sprite_ai/cractus.c'
ASM_OVERRIDES.update({
  (_CR, 1947): ('lsr %0, r0, #24', 'slot219 = (u32)(u8)SpriteUtilFindSprite(219, 0);'),
  (_CR, 2014): ('ldrb %0, [r1, #0]', '{ u8 *_w2 = (u8 *)((u32)current + 41); count = (u32)*_w2 - 1; *_w2 = (u8)count; }'),
  (_CR, 2370): ('sub r0, #4', 'sprite->xPosition -= 4;'),
  (_CR, 3646): ('add r1, r3, #0', 'gCurrentSprite.work3 = (u8)yDelta;'),
  (_CR, 3668): ('add r1, r3, #0', 'gCurrentSprite.work3 = (u8)yDelta;'),
})

# ---- hand-ported inline asm (bubble, toy_block_triangle, minicula, golden_diva) -----------------
# Straight register steering; semantics are in the source (struct offsets per PrimarySpriteData).
#   bubble 305/338/340   `strb r4,[%0]`: r4 is the pinned `zero` (register int zero asm("r4")) -> *bytePointer = (u8)zero
#   bubble 332           `strb r4,[r3,#22]` r3 = sprite -> sprite->currentAnimationFrame (0x16) = zero
#   toy_block_triangle 488  `mov r0,#17; strb r0,[%0,#28]` -> current->pose (0x1C) = 17
#   minicula 21          `add r0,r2,#0; mov r3,#0; mov r4,#0` with outs (newStatus, zero, zeroHalfword), in statusMask
#   golden_diva 141      `cmp r0,#81; blt 1f` (r0 = pose, signed) ... `1:`: skip the store when pose < 81
ASM_OVERRIDES.update({
  ('src/sprite_ai/bubble.c', 305): ('strb r4, [%0]', '*bytePointer = (u8)zero;'),
  ('src/sprite_ai/bubble.c', 332): ('strb r4, [r3, #22]', 'sprite->currentAnimationFrame = (u8)zero;'),
  ('src/sprite_ai/bubble.c', 338): ('strb r4, [%0]', '*bytePointer = (u8)zero;'),
  ('src/sprite_ai/bubble.c', 340): ('strb r4, [%0]', '*bytePointer = (u8)zero;'),
  ('src/sprite_ai/toy_block_triangle.c', 488): ('mov r0, #17', 'current->pose = 17;'),
  ('src/sprite_ai/minicula.c', 21): ('add r0, r2, #0', 'newStatus = statusMask; zero = 0; zeroHalfword = 0;'),
  ('src/sprite_ai/golden_diva.c', 141): ('cmp r0, #81', 'if ((s32)pose < 81) @@GOTO1@@;'),
})

# ---- hand-ported inline asm (cuckoo_condor, shopkeeper) ------------------------------------------
# Compare+branch "compiler boundaries" and one dead `add`.  Targets/signedness taken from the instruction (bls = unsigned,
# blt = signed) and from the C labels the branches land on.
#   cuckoo_condor 1761  `cmp value,#15; bls .LCuckooCondorClockPieceUpdateEnd` -> the function's `end:` (label sits right after it)
#   cuckoo_condor 4265  `cmp r0,#22; blt 1f`  -> pose < 22 (signed) goes to the `1:` at copyParent
#   cuckoo_condor 4268  `cmp r0,#105; blt 1f` -> pose < 105 goes to the same `1:`; 105/106 fall through to HeadAttack
#   shopkeeper 603      `add r0,#1`: C variable r0 is dead afterwards (checked), kept as r0 += 1
#   shopkeeper 4959     `cmp r0,#22; blt .L_testc1_case21` -> v < 22 (signed) to case21
#   shopkeeper 5197     `cmp r0,#1; blt .L_test51ed8_countdown` -> r0 (s32, gCurrentPassage) < 1 to countdown
ASM_OVERRIDES.update({
  ('src/sprite_ai/cuckoo_condor.c', 1761): ('bls .LCuckooCondorClockPieceUpdateEnd', 'if ((u32)value <= 15) goto end;'),
  ('src/sprite_ai/cuckoo_condor.c', 4265): ('cmp r0, #22', 'if ((s32)pose < 22) @@GOTO1@@;'),
  ('src/sprite_ai/cuckoo_condor.c', 4268): ('cmp r0, #105', 'if ((s32)pose < 105) @@GOTO1@@;'),
  ('src/sprite_ai/shopkeeper.c', 603): ('add r0, #1', 'r0 += 1;'),
  ('src/sprite_ai/shopkeeper.c', 4959): ('blt .L_testc1_case21', 'if ((s32)v < 22) goto _asm_L_testc1_case21;'),
  ('src/sprite_ai/shopkeeper.c', 5197): ('blt .L_test51ed8_countdown', 'if ((s32)r0 < 1) goto _asm_L_test51ed8_countdown;'),
})

# ---- hand-ported inline asm (game_screen_helpers, boss_door_opening) -----------------------------
#   game_screen_helpers 506   `mov %0,r0` = return value of the DecompressRoomBackground call on the line above (u32 return, no ABI
#                             change needed).  The bare call is folded into this assignment by a PATCHES rule (else it runs twice).
#   game_screen_helpers 1745  `mov r0,#128; orr %0,r0` -> nextState |= 0x80
#   boss_door_opening 1930    ops: r1 &= r8 (r8Work); r2 = oam->attr1 (ldrh [oam,#2]); r0 = ip (clearMask) & r2; r0 |= r1
#                             -> mergedX = (clearMask & oam attr1) | (newX & r8Work); both outputs (mergedX, oldX) assigned
ASM_OVERRIDES.update({
  ('src/game_screen_helpers.c', 506):  ('mov %0, r0', 'transferSize = (s32)DecompressRoomBackground(0, selectedData, overlayBuffer);'),
  ('src/game_screen_helpers.c', 1745): ('orr %0, r0', 'nextState |= 128;'),
  ('src/boss_door_opening.c', 1930):   ('mov r2, r8', 'oldX = *(u16 *)((u32)oam + 2); newX &= (u32)r8Work; mergedX = ((u32)clearMask & oldX) | newX;'),
})

# ---- hand-ported inline asm (pause_screen.c: RenderPauseScreenOam) ---------------------------------
# Checked against asm/disasm_pause_screen_RenderPauseScreenOam.s (the matching build of this function):
#   934   `ldr r5,[sp,#8]; add r0,r5,r0; ldr r3,[r0]`: [sp,#8] is the spilled `tableOffset` (disasm: `mov r0,#0; str r0,[sp,#8]`
#         before the loop); r0 = animation-pointer table base  ->  jewelAnimation = *(table + tableOffset).  The same offset is
#         used right after to index gPauseJewelAnimationStates (disasm: `add r5,r5,r2`).
#   969/1049/1079  `mov %0, ip`: ip is `nextSlot` (declared `register s32 nextSlot asm("r12")`; disasm: `add ip,r0` right after the
#         slot-count read, `mov r2,ip; cmp r2,#128`).
ASM_OVERRIDES.update({
  ('src/pause_screen.c', 934):  ('ldr %1, [sp, #8]', 'jewelAnimOffset = tableOffset; jewelAnimationBase = (const u8 *)((u32)jewelAnimationBase + (u32)jewelAnimOffset); jewelAnimation = *(const struct AnimationFrame * const *)jewelAnimationBase;'),
  ('src/pause_screen.c', 969):  ('mov %0, ip', 'jewelSlotCheck = nextSlot;'),
  ('src/pause_screen.c', 1049): ('mov %0, ip', 'jewel2SlotCheck = nextSlot;'),
  ('src/pause_screen.c', 1079): ('mov %0, ip', 'jewel2LowR2 = nextSlot;'),
})

# ---- hand-ported inline asm (tile_interaction.c: UpdateWarioEnvironmentalTiles) ----------------------
# `struct Frame` lives at sp+0 (u32 fields: bg0Attributes[2]@0, bg1Attributes[2]@8, bg2Attributes[2]@16, xTiles[2]@24, bg1Row@32,
# bg1Map@36, bg2Row@40, bg2Map@44, bg2AttributesPtr@48), which is what the stack offsets in the asm encode.  The asm used r8 = bg1Attributes
# and r5 = attributes (both are also passed as unused inputs); written with the names instead of registers.
#   2016  `mov %0,sp; add %0,#16` = &frame.bg2Attributes
#   2050  addr = bg1Map + value*2; value = attributes[*(u16*)addr]; bg1Attributes[offset/4] = value   (store at bg1Attributes + offset)
#   2067  same with bg2Map, and the store goes to frame.bg2AttributesPtr + offset
ASM_OVERRIDES.update({
  ('src/tile_interaction.c', 2016): ('mov %0, sp', 'bg2Ptr = frame.bg2Attributes;'),
  ('src/tile_interaction.c', 2050): ('ldr r1, [sp, #36]', 'value = (u32)*(u16 *)(frame.bg1Map + (value << 1)); value = (u32)*(u16 *)((u32)attributes + (value << 1)); *(u32 *)((u32)bg1Attributes + offset) = value;'),
  ('src/tile_interaction.c', 2067): ('ldr r1, [sp, #44]', 'value = (u32)*(u16 *)(frame.bg2Map + (value << 1)); value = (u32)*(u16 *)((u32)attributes + (value << 1)); *(u32 *)((u32)frame.bg2AttributesPtr + offset) = value;'),
})

def _macro_only(tpl):
    """True if the template consists only of `.macro ... .endm` definitions (nested ones included).
    The decomp uses these to re-steer the register/opcode of the *next* compiler-emitted instruction
    (`.macro mov ..; .purgem mov; mov r5,#0; .endm`).  They never carry semantics of their own: the C
    statement that follows is the real operation, so under clang they are no-ops."""
    depth, saw = 0, False
    for ln in re.split(r'\\n|\n', tpl):
        s_ = re.sub(r'^(?:\s|\\t)+', '', ln)
        if not s_: continue
        if s_.startswith('.macro'): depth += 1; saw = True; continue
        if s_.startswith('.endm'): depth -= 1; continue
        if depth <= 0: return False
    return saw and depth == 0

USED_OVERRIDES = set()

def fix_asm(m):
    tpl = ''.join(STR.findall(m.group(1)))
    cons = m.group(2) or ''
    _ln = CUR[1].count('\n', 0, m.start()) + 1
    _ov = ASM_OVERRIDES.get((CUR[0], _ln))
    if _ov:
        if _ov[0] not in tpl.replace('\\n', '\n').replace('\\t', ' '):
            raise SystemExit(f'ASM_OVERRIDES {CUR[0]}:{_ln}: marker {_ov[0]!r} not in template {tpl!r}')
        USED_OVERRIDES.add((CUR[0], _ln))
        return _ov[1]
    if tpl.strip() == '':
        # An empty template with a tied input (`: "=r"(out) : "0"(in)`) is an assignment `out = in`
        # in disguise (agbcc register steering).  Dropping it leaves `out` uninitialised and clang
        # then turns the later use into wasm `unreachable`.  Emit the assignment instead.
        parts = re.split(r'(?<!:):(?!:)', cons)
        if len(parts) >= 3:
            outs = [(c, e) for c, e in re.findall(r'"([^"]*)"\s*\(\s*([^()]*?)\s*\)', parts[1])]
            ins = re.findall(r'"([^"]*)"\s*\(\s*((?:[^()]|\([^()]*\))*?)\s*\)', parts[2])
            asg = []
            for c, e in ins:
                if c.isdigit() and int(c) < len(outs) and outs[int(c)][0].startswith('='):
                    asg.append(f'{outs[int(c)][1]} = {e};')
            if asg:
                TIED_ASM.append(f'{CUR[0]}:{_ln}')
                return ' '.join(asg)
        return ';'                                    # pure optimizer barrier
    if _macro_only(tpl):
        MACRO_NOOPS.append(f'{CUR[0]}:{_ln}')
        return ';'                                    # agbcc register-steering macros
    if re.fullmatch(r'\s*@[^\n]*(?:\\n\s*@[^\n]*)*', tpl.replace('\\n', '\n')) and tpl.strip():
        return ';'                                    # assembler comment used as a position marker
    if re.fullmatch(r'[A-Za-z_]\w*', tpl.strip()):
        return m.group(0)                             # asm("symbol") label on a declaration
    if re.fullmatch(REG_NAMES, tpl.strip()):
        return m.group(0)                             # register pin declaration, stripped later
    # assembler-only layout/encoding directives (pool placement, mapping symbols, branch-shape macros)
    flat = tpl.replace('\\n', '\n')
    if re.fullmatch(r'(\s*(\.align[^\n]*|\.pool|\.set\s+\$[^\n]*|\.macro[^\n]*|\.purgem[^\n]*|\.endm|\.set\s+\.L[^\n]*|bne\s+\\\\target|b\s+\.L52_saved_reset|\n))*', flat) and flat.strip():
        return ';'
    lm = LABEL1.fullmatch(tpl)
    if lm: return '@@LABEL1@@' if lm.group(1) == '1' else '_asm' + lm.group(1).replace('.', '_') + ':;'
    jm = JMP.fullmatch(tpl)
    if jm: return ('@@GOTO1@@;' if jm.group(1) == '1f' else goto_tok(jm.group(1)))
    cb = CMPBR.fullmatch(tpl.replace('\\n', '\n').replace('\\t', ' '))
    if cb:
        ops = parse_operands(cons)
        return f'if ((s32)({ops[int(cb.group(1)[1:])][1]}) {BR[cb.group(3)]} {cb.group(2)}) ' + goto_tok(cb.group(4)).rstrip(';') + ';'
    sv = re.fullmatch(r'\s*(?:svc|swi)\s+(0x[0-9A-Fa-f]+|\d+)\s*', tpl)
    if sv: return f'hal_syscall({int(sv.group(1), 0)});'
    t = translate(tpl, cons)
    if t is not None:
        # Audit: a successful translation must assign every pure output ("=r") operand.  Dropping one is the failure mode
        # that silently broke draw_roulette.c 150/831/935 (result went into a block-local temp instead of %0).
        # ("+r" operands are often address-only inputs, so they are not checked.)
        for _c, _e in parse_operands(cons):
            if _c.startswith('=') and not re.search(re.escape(_e) + r' = ', t):
                AUDIT.append(f"{CUR[0]}:{CUR[1].count(chr(10), 0, m.start()) + 1}\toutput {_e!r} never assigned")
        return t
    line = CUR[1].count('\n', 0, m.start()) + 1
    untranslated[(CUR[0], line, re.sub(r'\s+', ' ', tpl)[:100])] += 1
    if STRICT: return m.group(0)
    # Keep the file compiling: the site becomes a logged no-op.  Outputs of the asm stay
    # unassigned, so behaviour there is WRONG until the site is ported by hand.
    SITE_IDS.append(f'{CUR[0]}:{line}')
    return f'hal_unported_asm({len(SITE_IDS) - 1});'

# agbcc lets these locals be assigned after a const declaration; clang does not
# WIP/NON_MATCHING C declares scalar locals `const T x;` and assigns them later (agbcc accepts it)
CONST_SCALAR_LOCAL = re.compile(r'^(\s+)const\s+((?:u8|u16|u32|s8|s16|s32|int|unsigned int|unsigned)\s+\w+;)', re.M)
CONST_LOCAL = re.compile(r'\bconst\s+(u16\s+(?:previousMovement|new_var2)\b)')
DECL = re.compile(r'(\w+)\s*(?:\[[^\]]*\]\s*)*asm\("(' + REG_NAMES + r')"\)')
FUNC_END = re.compile(r'^\}', re.M)

def resolve_labels(text):
    # `bCC 1f` -> goto the next `1:` that follows it in the file (same function in practice)
    out = []; pos = 0; labels = [m.start() for m in re.finditer(r'@@LABEL1@@', text)]
    n = 0
    def lab(i): return f'_asm_l{i}'
    res = text
    # number labels in order; each goto picks the first label after it
    idx = {p: i for i, p in enumerate(labels)}
    def goto(m):
        for p in labels:
            if p > m.start(): return f'goto {lab(idx[p])}'
        return 'goto _asm_missing'
    res = re.sub(r'@@GOTO1@@', goto, res)
    # label statements: replace by position order
    cnt = [0]
    def lbl(m): i = cnt[0]; cnt[0] += 1; return f'{lab(i)}:;'
    return re.sub(r'@@LABEL1@@\s*;?', lbl, res)

def block_end(text, pos):
    """index of the '}' closing the innermost block that contains pos (comments/strings skipped)."""
    depth = 0; i = pos; n = len(text)
    while i < n:
        c = text[i]
        if text.startswith('/*', i): i = text.find('*/', i) + 2; continue
        if text.startswith('//', i): i = text.find('\n', i); i = n if i < 0 else i; continue
        if c in '"\'':
            q = c; i += 1
            while i < n and text[i] != q: i += 2 if text[i] == '\\' else 1
            i += 1; continue
        if c == '{': depth += 1
        elif c == '}':
            if depth == 0: return i
            depth -= 1
        i += 1
    return n

def fix(text):
    # A pinned declaration `T x asm("rN")` makes literal `rN` in later asm templates mean x,
    # but only inside the block that declares it.
    decls = []
    CUR[1] = text
    for m in DECL.finditer(text):
        decls.append((m.start(), block_end(text, m.end()), ALIAS.get(m.group(2), m.group(2)), m.group(1)))
    repl = []
    for m in ASM_STMT.finditer(text):
        REGMAP.clear()
        for ds, de, reg, var in decls:
            if ds < m.start() < de: REGMAP[reg] = var
        repl.append((m.start(), m.end(), fix_asm(m)))
    for a, b, r in reversed(repl):
        text = text[:a] + r + text[b:]
    text = resolve_labels(text)
    text = CONST_SCALAR_LOCAL.sub(r'\1\2', text)
    text = ASM_REG.sub('', text)
    text = CONST_LOCAL.sub(r'\1', text)
    return REGISTER.sub('', text)

DMASET = re.compile(r'#define DmaSet\(dmaNum, src, dest, control\)\s*\\\n\{.*?\n\}\n', re.S)
DMASET_NEW = ('#define DmaSet(dmaNum, src, dest, control) \\\n'
              '    { hal_dma_set(dmaNum, (unsigned)(src), (unsigned)(dest), (unsigned)(control)); }\n')

# The ROM reuses some addresses with different prototypes (extra register args are simply
# ignored on ARM).  wasm checks signatures, so make every use agree.
IGNORE_ARGS = ('WaitForDma3Transfer', 'func_8000F90')
IGN = re.compile(r'\b(' + '|'.join(IGNORE_ARGS) + r')\s*\(((?:[^()]|\([^()]*\))+)\)')
PATCHES = [   # (relative path, regex, replacement)
    # --- ABI mismatches: wasm validates signatures, ARM silently ignored extra args / unused returns
    ('src/cutscenes.c', r'\A', 'void SubGameClearGraphicsMemory(void); void StartDemoPlayback(void); void InterruptCallbackSetVCount(void (*cb)(void));\n'),
    ('include/minigame_transition.h', r'void WaitForVBlankInterrupt\([^)]*\);', 'void WaitForVBlankInterrupt(void);'),
    ('src/minigame_transition.c', r'WaitForVBlankInterrupt\(subGameMode\);', 'WaitForVBlankInterrupt();'),
    ('src/title.c', r'^void BuildCutsceneBackgroundAffineMatrix\(', 'u16 *BuildCutsceneBackgroundAffineMatrix('),
    ('src/sprite_collision.c', r'\A', 'void func_8023BFC(u16 y, u16 x);\n'),
    ('src/passage_screen.c', r'\A', 'void BuildTemporaryStageSelectionSave(void);\n'),
    ('src/minigames/homerun_derby.c', r'\A', 's32 UpdateHomerunBonusAnimation(void); void DrawHomerunDerbyScore(void); void StartHomerunBonusAnimation(void); void ResetHomerunDerbyPitch(void);\nvoid SelectHomerunPitchPath(void); void UpdateHomerunBatInput(void);\n'),
    # DrawHomerunDerby (static-OAM block): the original reads the attr1 mask from the register `sl` that the preceding asm set to -15.
    # The port turns that asm into `oamByteMask = -15`, but the C reads the unrelated, stale `oamXMask` (0x1FF), which corrupts the
    # pitcher's OAM attr byte.  Single-line replacement on purpose: ASM_OVERRIDES are keyed by ORIGINAL line numbers.
    ('src/minigames/homerun_derby.c', r'(workingOffset = )oamXMask;(\s*\n\s*workingOffset &= pitcherAnimationType;)', r'\1oamByteMask;\2'),
    ('src/sprite_collision.c', r'gSpriteCollisionFlags & COLLISION_BELOW\b', 'gSpriteCollisionFlags & SPRITE_COLLISION_BELOW'),
    ('src/sprite_collision.c', r'(gSpriteData\[slot\]\.pose = SPOSE_PUSHED_RIGHT_INIT)\s*\n', r'\1;\n'),
    ('src/sprite_ai/moguramen.c', r'extern void func_8023BFC\(\);', 'extern void func_8023BFC(u16, u16);'),
    # --- call_indirect return-type mismatch: these four tables hold void(void) functions but wario.h declares them
    # as u8(*)(void).  ARM ignores the unused r0; wasm traps (trap 6) on the signature mismatch in FinalizeWarioUpdate.
    ('include/wario.h', r'^(typedef void \(\*WarioInteractionFunc\)\(u8\);)', r'\1\ntypedef void (*WarioVoidFunc)(void);'),
    ('include/wario.h', r'^extern WarioPoseFunc (sUnk_82DE(?:CD0|D00|D60|D90))\[\];', r'extern WarioVoidFunc \1[];'),
    # Diagnostic + guard: log the pose before the indirect call (host prints it when it changes) and refuse an index past the
    # 79-slot ROM table instead of trapping.  The last "trace[1]" line before a call_indirect trap names the offending pose.
    ('src/wario/normal.c', r'pose = sWarioNormalPoseTable\[gWarioData\.pose\]\(\);',
     '{ u32 poseIdx = gWarioData.pose; hal_trace_val(1, poseIdx); if (poseIdx >= 79) { pose = 0xFF; } else { pose = sWarioNormalPoseTable[poseIdx](); } }'),
    # --- uninitialised register read: the original asm("" : "=r"(p)) just named r4 (= &gStageEntrySelectedStage);
    # with the asm gone `p` is undef and clang turns the *p path into wasm `unreachable` (trap 5 on stage entry).
    ('src/stage_entry.c', r'(u8 \*normalSelectedStage);(\s*\n\s*);', r'\1 = &gStageEntrySelectedStage;\2'),
    ('src/sprite_ai/cractus.c', r'^    SpriteUtilFindSprite\(219, 0\);\n(?=    /\* Preserve the signed-return)', ''),
    ('src/game_screen_helpers.c', r'^            DecompressRoomBackground\(0, selectedData, overlayBuffer\);\n(?=\s*/\* Preserve the decompressed size)', ''),
    ('src/wario/puffy.c', r'^void ResolveWarioFloorCollision\(void\);', 'u8 ResolveWarioFloorCollision(void);'),
    # SampleFreqSet busy-waits on VCOUNT through a raw memory read; nothing advances VCOUNT in wasm memory, so it spun forever
    # (black screen, frames=0).  Read it through the HAL, which steps the scanline.
    # BuildMainSaveWorkingBuffer is void; the old fn-pointer cast read a stale r0.  A plain call can't assign a void result.
    ('src/minigame.c', r'result = BuildMainSaveWorkingBuffer\(\);', 'BuildMainSaveWorkingBuffer(); result = 0;'),
    ('src/m4a.c', r'\*\(vu8 \*\)REG_ADDR_VCOUNT', '((u8)hal_poll_vcount())'),
    # SoundMainBTM is really Clear64byte (called as void(void *) through the jump table): wasm checks indirect-call signatures
    ('include/gba/m4a.h', r'void SoundMainBTM\(void\);', 'void SoundMainBTM(void *);'),
    # linker.ld `gNumMusicPlayers = 8; gMaxLines = 70;` are absolute *values* used through their address; undefined symbols were 0
    ('include/gba/m4a.h', r'#define NUM_MUSIC_PLAYERS \(\(u16\)gNumMusicPlayers\)', '#define NUM_MUSIC_PLAYERS 8'),
    ('include/gba/m4a.h', r'#define MAX_LINES \(\(u32\)gMaxLines\)', '#define MAX_LINES 70'),
    # linker.ld `sRouletteInitialTileSin = sSinCosTable + 0x1C0;` (an address inside the sine table)
    ('include/minigames/roulette.h', r'extern const s16 sRouletteInitialTileSin;', '#include "fixed_point.h"\n#define sRouletteInitialTileSin (*(const s16 *)((const char *)sSinCosTable + 0x1C0))'),
    # same class of bug: raw busy-waits on registers only the HAL can advance
    ('src/file_select.c', r'\(u16\)\(\*\(vu16 \*\)0x04000006 - 21\)', '(u16)(hal_poll_vcount() - 21)'),
    # HBlank callbacks: the GBA build DMAs the callback's *code* into an IWRAM trampoline (gHBlankCallbackTrampoline) and
    # registers `trampoline|1`.  In wasm a function pointer is a table index, so the DMA copied 512 junk bytes over IWRAM
    # and the HBlank IRQ did call_indirect on 0x0300322d (wasm trap 6).  Register the real function directly.
    ('src/hblank.c', r'(?s)\(\(volatile struct Dma3Regs_HBlankCallbackCopy\*\)0x040000D4\)->src = callback;.*?InterruptCallbackSetHBlank\(\(void \(\*\)\(\)\)\(1 \| \(\(s32\)\(&gHBlankCallbackTrampoline\)\)\)\);', 'InterruptCallbackSetHBlank(callback);'),
    # diagnostics: log changes of main/sub game mode, stage-exit step and exit type (tags 2..5 of hal_trace_val)
    ('src/main.c', r'^(\s*)TIMER_COUNT_UP\(gMainTimer\);', r'\1TIMER_COUNT_UP(gMainTimer);\n\1{ hal_trace_val(2, (unsigned)gMainGameMode); hal_trace_val(3, (unsigned)(u16)gSubGameMode); hal_trace_val(4, (unsigned)gSpriteAiDropTimer); hal_trace_val(5, (unsigned)gStageExitType); }'),
    # diagnostics for the vortex exit: tag 6 = gCollectedKeyzer when the portal is entered, tag 7 = entering-sprite countdown
    ('src/sprite_ai/vortex.c', r'^(\s*)gUnk_3000C0E = 1;', r'\1gUnk_3000C0E = 1;\n\1{ hal_trace_val(6, 100 + (unsigned)gCollectedKeyzer); }'),
    ('src/sprite_ai/vortex.c', r'(void SpriteWarioEnteringVortex\(void\)\n\{\n(?:.*\n)*?\s*TIMER_COUNT_DOWN\(gCurrentSprite\.work0\);)', r'\1\n            { hal_trace_val(7, (unsigned)gCurrentSprite.work0); }'),
    # vortex exit: only SpriteWarioEnteringVortex is ever spawned (VortexFinishStage), but it started the stage-exit sequence
    # (gSubGameMode = 6) just when gCollectedKeyzer != 1 -- with the Keyzer collected nothing did, Wario stayed disabled for the
    # whole 16.7 s pause and the timer ran out.  Start the exit unconditionally (the Keyzer sprite sets the very same values).
    ('src/sprite_ai/vortex.c', r'if \(gCollectedKeyzer != 1\) \{\n(\s*gSubGameMode = 6;\n\s*gSpriteAiDropTimer = 0;\n\s*gStageExitType = 2;\n)(\s*)\}', r'{\n\1\2}'),
    # DrawHomerunDerby keeps `register u8 *stackFrame asm("sp")` and addresses affine PB/PC/PD as sp+2/+4/+6, i.e. the elements
    # after the local u16 affineMatrix[0] at the bottom of its frame.  Under wasm the `sp` register variable is undefined, so
    # clang turned every use into `unreachable` (wasm trap 5 in the Home Run Derby minigame).  Point it at the real array.
    ('src/minigames/homerun_derby.c', r'(?<!\*)\bstackFrame\b', '((u8 *) affineMatrix)'),
    # defensive: these pick a table with `if (x == 1) p = A; if (x == 2) p = B;` and read p unconditionally.  On ARM an unexpected
    # x used a stale register; under clang an uninitialised read is undef and can collapse the path to `unreachable`.  Make the
    # second test a catch-all so p is always set (identical for the expected values).  Found with gcc -Wmaybe-uninitialized.
    ('src/minigames/homerun_derby.c', r'if \(byteValue == 2\)(\s*\{\s*frameData = sHomerunCameraOamMode2;)', r'if (byteValue != 1)\1'),
    ('src/minigames/roulette/draw_roulette.c', r'if \(active == 2\)(\s*src = sRouletteMainState2Oam)', r'if (active != 1)\1'),
    ('src/minigames/wario_hop.c', r'if \(active == 2\) \{(\s*frame = sUnk_870D890;)', r'if (active != 1) {\1'),
    # UpdateRouletteResultTiles only sets src/dst for states 7/9/11 and then copies through them unconditionally
    ('src/minigames/roulette/update_roulette_result_tiles.c', r'(CALC_SOURCE\(gRouletteBottomResult, sRouletteBottomResultTiles, 0x06017200\);\s*break;)', r'\1\n    default:\n        return;'),
    ('src/credits.c', r'while \(\(\*\(vu16 \*\)0x04000004 & 2\) == 0\) \{\s*\}', '/* port: HBlank-flag spin skipped (the HAL calls this at HBlank time) */'),
]

# ---- linker.ld symbols ------------------------------------------------------------------------------
# The ROM build gives ~550 IWRAM variables their address in linker.ld (`. = 0x0018; gResetSaveFile = .;`) and the C
# only has `extern` declarations.  wasm-ld has no such script: with --allow-undefined every one of them resolved to
# address 0, so gResetSaveFile, gRandomSeed, gCurrentStageID ... all aliased the same bytes (AgbMain's
# `gRandomSeed += 1` made gResetSaveFile non-zero on the 2nd frame and the main loop exited -> black screen).
# Rewrite each such `extern T name;` / `extern T name[N];` into a macro that accesses the real GBA IWRAM address,
# the same place raw `*(u16 *)0x03000xxx` accesses and DMA/BIOS fills see.
IWRAM_BASE = 0x03000000
_ld = (root / 'linker.ld').read_text()
_iw = _ld[_ld.index('iwram (NOLOAD)'):]; _iw = _iw[:_iw.index('} > IWRAM')]
LDSYMS = {n: IWRAM_BASE + int(a, 16) for a, n in re.findall(r'\.\s*=\s*(0x[0-9a-fA-F]+);\s*(\w+)\s*=\s*\.;', _iw)}
LD_DECL = re.compile(r'^[ \t]*extern[ \t]+([^;(){},]*?)[ \t]*\b(\w+)\b((?:[ \t]*\[[^\]]*\])*)[ \t]*;[ \t]*$', re.M)
LD_REWRITTEN = Counter()
def _ld_macro(m):
    ty, name, dims = m.group(1).strip(), m.group(2), re.findall(r'\[[^\]]*\]', m.group(3))
    if name not in LDSYMS or not ty: return m.group(0)
    addr = '0x%08X' % LDSYMS[name]
    if not dims: body = f'(*({ty} *){addr})'
    elif len(dims) == 1: body = f'(({ty} *){addr})'
    else: body = f'(({ty} (*){"".join(dims[1:])}){addr})'
    LD_REWRITTEN[name] += 1
    return f'#undef {name}\n#define {name} {body}'

# ---- function-pointer-cast calls ------------------------------------------------------------------
# The decomp writes `((int (*)(u8, u16, u8))CheckWarioVerticalCollision)(a, b, c)` to coax agbcc into a particular
# register allocation.  Under clang the call goes through a prototype whose parameter types differ from the callee's
# (u8 / s32 vs u32 / u16): wasm-ld cannot bitcast that and replaces the call by `<fn>_bitcast_invalid`, which is just
# `unreachable` (wasm trap 5, e.g. CutsceneWarioDrawPoseOam in the intro cutscene).  A plain call lets C convert the args.
CASTCALL = re.compile(r'\(\(\s*[\w\s\*]+?\(\*\)\s*\([^()]*\)\s*\)\s*(\w+)\s*\)\s*\(')
PAD_ARGS = {'SpawnHighPriorityPrimarySprite': 5}      # callers pass 4 args (5th was a stale register on ARM)
CASTCALL_FIXED = Counter()
def _balanced_args(text, i):
    """text[i] is just after '(' -> (args list, index after the matching ')')"""
    depth, j, cur, args = 1, i, '', []
    while j < len(text) and depth:
        c = text[j]
        if c == '(': depth += 1
        elif c == ')':
            depth -= 1
            if depth == 0: break
        if c == ',' and depth == 1: args.append(cur); cur = ''
        else: cur += c
        j += 1
    if cur.strip() or args: args.append(cur)
    return args, j + 1
def fix_castcalls(text):
    out, pos = [], 0
    for m in CASTCALL.finditer(text):
        if m.start() < pos: continue
        name = m.group(1)
        out.append(text[pos:m.start()]); CASTCALL_FIXED[name] += 1
        if name in PAD_ARGS:
            args, end = _balanced_args(text, m.end())
            while len(args) < PAD_ARGS[name]: args.append(' 0')
            out.append(name + '(' + ','.join(args) + ')'); pos = end
        else:
            out.append(name + '('); pos = m.end()
    out.append(text[pos:])
    return ''.join(out)

n = 0
for sub in ("src", "include"):
    for p in (root / sub).rglob("*"):
        if p.is_dir(): continue
        dst = out / p.relative_to(root)
        dst.parent.mkdir(parents=True, exist_ok=True)
        if p.suffix in (".c", ".h"):
            ovf = root / 'port' / 'override' / p.relative_to(root)
            text = (ovf if ovf.exists() else p).read_text(errors="replace")
            if p.name == "io_reg.h":
                text = text.replace('#define REG_VCOUNT      (*(vu16 *)REG_ADDR_VCOUNT)', '#define REG_VCOUNT      hal_poll_vcount()')
                text = '#include "port.h"\n' + text
            if p.name == "macro.h":
                text = DMASET.sub(lambda m: DMASET_NEW, text)
                text = '#include "port.h"\n' + text
            CUR[0] = str(p.relative_to(root)); text = fix(text); n += 1
            text = fix_castcalls(text)
            text = IGN.sub(lambda m: m.group(1) + '()' if m.group(2).strip() != 'void' else m.group(0), text)
            for rp, rx, rep in PATCHES:
                if CUR[0] == rp: text = re.sub(rx, rep, text, flags=re.M)
            text = LD_DECL.sub(_ld_macro, text)
            dst.write_text(text)
        else:
            shutil.copy2(p, dst)
# overlay hand-written replacements (compiler.h, port.h, ...)
ov = root / "port" / "include"
for p in ov.rglob("*"):
    if p.is_file():
        d = out / "include" / p.relative_to(ov); d.parent.mkdir(parents=True, exist_ok=True); shutil.copy2(p, d)
_missing = sorted(set(ASM_OVERRIDES) - USED_OVERRIDES)
if _missing:
    raise SystemExit(f'ASM_OVERRIDES never matched: {_missing}')
(out / "ASM_SITES.txt").write_text("\n".join(f"{i}\t{x}" for i, x in enumerate(SITE_IDS)) + "\n")
(out / "AUDIT.txt").write_text("\n".join(AUDIT) + "\n")
(out / "UNTRANSLATED.txt").write_text(
    "\n".join(f"{f}:{l}\t{t}" for (f, l, t), c in sorted(untranslated.items())) + "\n")
_miss = sorted(set(LDSYMS) - set(LD_REWRITTEN))
(out / "LDSYMS_UNREWRITTEN.txt").write_text("\n".join(_miss) + "\n")
print(f"linker.ld symbols: {len(LD_REWRITTEN)}/{len(LDSYMS)} rewritten to real IWRAM addresses ({len(_miss)} never declared, see LDSYMS_UNREWRITTEN.txt)")
print(f"tied empty-asm sites rewritten to assignments: {len(TIED_ASM)}")
print(f"cast calls rewritten to plain calls: {sum(CASTCALL_FIXED.values())} ({dict(CASTCALL_FIXED)})")
print(f"audit: {len(AUDIT)} translated asm site(s) dropped an output operand (see AUDIT.txt)")
print(f"portified {n} files -> {out}; {len(untranslated)} inline-asm sites left for manual port (see UNTRANSLATED.txt)")
