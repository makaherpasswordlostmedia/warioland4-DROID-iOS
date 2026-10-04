/* Tiny freestanding runtime for the wasm32 module (compiled with -mbulk-memory). */
typedef unsigned int size_t_;
void *memcpy(void *d, const void *s, size_t_ n) { __builtin_memcpy(d, s, n); return d; }
void *memmove(void *d, const void *s, size_t_ n) { __builtin_memmove(d, s, n); return d; }
void *memset(void *d, int c, size_t_ n) { __builtin_memset(d, c, n); return d; }
/* ARM libgcc helpers the decomp links against; GBA BIOS Div returns garbage-but-defined for /0 */
int __divsi3(int a, int b) { return b ? (a == (-2147483647 - 1) && b == -1 ? a : a / b) : (a < 0 ? -1 : 1); }
int __modsi3(int a, int b) { return b ? (b == -1 ? 0 : a % b) : a; }
unsigned __udivsi3(unsigned a, unsigned b) { return b ? a / b : 0xFFFFFFFFu; }
unsigned __umodsi3(unsigned a, unsigned b) { return b ? a % b : a; }

#include "gba/m4a.h"   /* MPlayFunc + the exact prototypes of the two functions below */
/* ---- m4a helpers that were ARM/Thumb asm in the ROM build (asm/m4a_asm.s) ---------------------------------------- */
extern void *const sMPlayJumpTableTemplate[];
/* copy the 36 music-player command handlers into the RAM jump table (asm: `movs r1,#0x24` loop) */
void MPlayJumpTableCopy(MPlayFunc *dst) { for (int i = 0; i < 0x24; i++) dst[i] = (MPlayFunc)sMPlayJumpTableTemplate[i]; }
/* high 32 bits of a 32x32 -> 64 bit unsigned multiply (asm: umull) */
unsigned umul3232H32(unsigned a, unsigned b) { return (unsigned)(((unsigned long long)a * b) >> 32); }

/* ---- jump-table targets that Clear64byte()/ClearChain() reach through call_indirect ------------------------------
 * wasm checks the callee's signature on every indirect call.  These two were asm-only (= wasm imports whose declared
 * type did not match the call site: SoundMainBTM was declared `void(void)` but is called as `void(void *)`), so
 * m4aSoundInit -> MPlayOpen -> Clear64byte trapped with CALL_INDIRECT as soon as NUM_MUSIC_PLAYERS became non-zero. */
/* asm SoundMainBTM: zero 64 bytes at r0 (it is the m4a "Clear64byte" routine, not the mixer) */
void SoundMainBTM(void *dst) { unsigned *p = (unsigned *)dst; for (int i = 0; i < 16; i++) p[i] = 0; }
/* asm RealClearChain: unlink a note channel from its track's doubly linked chain */
void RealClearChain(void *x) {
    unsigned char *c = (unsigned char *)x;
    unsigned parent = *(unsigned *)(c + 0x2C);
    if (!parent) return;
    unsigned next = *(unsigned *)(c + 0x34), prev = *(unsigned *)(c + 0x30);
    if (prev) *(unsigned *)(prev + 0x34) = next; else *(unsigned *)(parent + 0x20) = next;
    if (next) *(unsigned *)(next + 0x30) = prev;
    *(unsigned *)(c + 0x2C) = 0;
}
/* ======================================================================================================================
 * m4a sequencer + PCM mixer, ported from asm/m4a_asm.s (MPlayMain, TrackStop, ply_*, SoundMain, m4aSoundVSync).
 * Field offsets were checked against the asm; the structs come from include/gba/m4a.h so layouts are identical.
 * Differences from the ROM build:
 *   - the mixer works on plain ints and clamps instead of the packed 8-bit-lane accumulator trick (no wrap-around crackle),
 *   - the maxLines "give up mixing when the scanline passed" deadline is dropped (no real-time limit here),
 *   - there is no DMA-to-FIFO: after every SoundMain() the new frame is handed to the host with hal_audio_push().
 * ==================================================================================================================== */
#define SOUND_INFO_ADDR (*(struct SoundInfo **)0x3007FF0)
extern const u8 sClockTable[];
u32 MidiKeyToFreq(struct WaveData *wav, u8 key, u8 fineAdjust);
extern void hal_audio_push(const void *right, const void *left, int count, int rate);   /* host: wl4 audio ring */

#define CH_ACTIVE 0xC7
#define CH_START  0x80
#define CH_STOP   0x40
#define CH_LOOP   0x10
#define CH_IEC    0x04

typedef struct SoundChannel Chan;
typedef struct MusicPlayerTrack Trk;
typedef struct MusicPlayerInfo MPlay;

/* ---- m4aSoundVSync: only the DMA-period counter survives (it selects which slot of the PCM ring SoundMain writes) ---- */
void m4aSoundVSync(void) {
    struct SoundInfo *si = SOUND_INFO_ADDR;
    u32 d = si->ident - ID_NUMBER;
    if (d > 1) return;
    int c = (int)si->pcmDmaCounter - 1;
    si->pcmDmaCounter = (u8)c;
    if (c > 0) return;
    si->pcmDmaCounter = si->pcmDmaPeriod;
}

/* ---- PCM mixer ---------------------------------------------------------------------------------------------------- */
static int sAccR[PCM_DMA_BUF_SIZE], sAccL[PCM_DMA_BUF_SIZE];

static void MixChannels(struct SoundInfo *si, int n) {
    u32 divFreq = (u32)si->divFreq;
    int mv = si->masterVolume;
    Chan *ch = si->chans;
    for (int c = 0; c < si->maxChans && c < MAX_DIRECTSOUND_CHANNELS; c++, ch++) {
        u32 st = ch->status;
        struct WaveData *wav = ch->wav;
        u32 ev;
        if (!(st & CH_ACTIVE)) continue;
        if (st & CH_START) {
            if (st & CH_STOP) { ch->status = 0; continue; }
            st = 3; ch->status = 3;
            ch->cp = (u32)wav + 0x10;
            ch->ct = wav->size;
            ev = 0; ch->ev = 0; ch->fw = 0;
            if ((wav->status >> 8) & 0xC0) { st |= CH_LOOP; ch->status = (u8)st; }
            goto attack;
        }
        ev = ch->ev;
        if (st & CH_IEC) {
            u8 old = ch->echoLength; ch->echoLength = (u8)(old - 1);
            if (old < 2) { ch->status = 0; continue; }
        } else if (st & CH_STOP) {                                    /* release */
            ev = (ev * ch->release) >> 8;
            if (ev <= ch->echoVolume) {
                ev = ch->echoVolume;
                if (ev == 0) { ch->status = 0; continue; }
                st |= CH_IEC; ch->status = (u8)st;
            }
        } else if ((st & 3) == 2) {                                   /* decay */
            ev = (ev * ch->decay) >> 8;
            if (ev <= ch->sustain) {
                ev = ch->sustain;
                if (ev == 0) {
                    ev = ch->echoVolume;
                    if (ev == 0) { ch->status = 0; continue; }
                    st |= CH_IEC; ch->status = (u8)st;
                } else { st--; ch->status = (u8)st; }
            }
        } else if ((st & 3) == 3) {                                   /* attack */
        attack:
            ev += ch->attack;
            if (ev >= 0xFF) { ev = 0xFF; st--; ch->status = (u8)st; }
        }
        ch->ev = (u8)ev;
        {
            int scaled = ((mv + 1) * (int)ev) >> 4;
            ch->er = (u8)((ch->rightVolume * scaled) >> 8);
            ch->el = (u8)((ch->leftVolume * scaled) >> 8);
        }
        st = ch->status;
        const s8 *loopPtr = 0; int loopLen = 0;
        if (st & CH_LOOP) { loopPtr = (const s8 *)((u32)wav + 0x10 + wav->loopStart); loopLen = (int)(wav->size - wav->loopStart); }
        {
            int er = ch->er, el = ch->el, ct = (int)ch->ct, o = 0, stopped = 0;
            const s8 *p = (const s8 *)ch->cp;
            if (ct <= 0 && !loopLen) { ch->status = 0; continue; }
            if (ch->type & TONEDATA_TYPE_FIX) {                       /* fixed-rate sample: 1 sample per output sample */
                while (o < n) {
                    int s = *p++; ct--;
                    sAccR[o] += (er * s) >> 8; sAccL[o] += (el * s) >> 8; o++;
                    if (ct == 0) {
                        if (loopLen) { ct = loopLen; p = loopPtr; } else { stopped = 1; break; }
                    }
                }
            } else {                                                  /* resampled: 23-bit fractional position */
                u32 fw = ch->fw, step = divFreq * ch->freq;
                int s0 = p[0], s1 = p[1];
                while (o < n) {
                    int s = s0 + (int)(((int)fw * (s1 - s0)) >> 23);
                    sAccR[o] += (er * s) >> 8; sAccL[o] += (el * s) >> 8; o++;
                    fw += step;
                    u32 adv = fw >> 23;
                    if (adv) {
                        fw &= 0x7FFFFF; ct -= (int)adv;
                        if (ct <= 0) {
                            if (!loopLen) { stopped = 1; break; }
                            int over = -ct; ct += loopLen;
                            while (ct <= 0) { ct += loopLen; over -= loopLen; }
                            p = loopPtr + over;
                        } else p += adv;
                        s0 = p[0]; s1 = p[1];
                    }
                }
                ch->fw = fw;
            }
            if (stopped) ch->status = 0;
            else { ch->ct = (u32)ct; ch->cp = (u32)p; }
        }
    }
}

void SoundMain(void) {
    struct SoundInfo *si = SOUND_INFO_ADDR;
    if (si->ident != ID_NUMBER) return;
    si->ident++;
    if (si->func) ((void (*)(u32))si->func)(si->intp);               /* chain of MPlayMain()s (sequencer ticks) */
    if (si->CgbSound) si->CgbSound();
    int n = si->pcmSamplesPerVBlank;
    if (n > 0 && n <= PCM_DMA_BUF_SIZE) {
        int counter = si->pcmDmaCounter;
        s8 *r = si->pcmBuffer, *nextR;
        if (counter - 1 > 0) r += (si->pcmDmaPeriod - (counter - 1)) * n;
        nextR = counter == 2 ? si->pcmBuffer : r + n;
        s8 *l = r + PCM_DMA_BUF_SIZE, *nextL = nextR + PCM_DMA_BUF_SIZE;
        int rev = si->reverb;
        if (rev) {
            for (int i = 0; i < n; i++) {
                int v = ((r[i] + l[i] + nextR[i] + nextL[i]) * rev) >> 9;
                if (v & 0x80) v++;
                sAccR[i] = sAccL[i] = (s8)v;
            }
        } else for (int i = 0; i < n; i++) sAccR[i] = sAccL[i] = 0;
        MixChannels(si, n);
        for (int i = 0; i < n; i++) {
            int a = sAccR[i], b = sAccL[i];
            r[i] = (s8)(a > 127 ? 127 : a < -128 ? -128 : a);
            l[i] = (s8)(b > 127 ? 127 : b < -128 ? -128 : b);
        }
        hal_audio_push(r, l, n, si->pcmFreq);
    }
    si->ident = ID_NUMBER;
}

/* ---- sequencer ---------------------------------------------------------------------------------------------------- */
static void clear_modM(Trk *t) { t->modM = 0; t->lfoSpeedC = 0; t->flags |= (t->modT == 0) ? 0xC : 3; }
static void ChnVolSetAsm(Chan *ch, Trk *t) {
    int ve = ch->ve, pan = (s8)ch->rp;
    int r = (t->volMR * (ve * (0x80 + pan))) >> 14, l = (t->volML * (ve * (0x7F - pan))) >> 14;
    ch->rightVolume = (u8)(r > 0xFF ? 0xFF : r);
    ch->leftVolume = (u8)(l > 0xFF ? 0xFF : l);
}

void TrackStop(MPlay *mp, Trk *t) {
    (void)mp;
    if (!(t->flags & MPT_FLG_EXIST)) return;
    for (Chan *c = t->chan; c; c = (Chan *)c->np) {
        if (c->status != 0) {
            if (c->type & 7) SOUND_INFO_ADDR->CgbOscOff(c->type & 7);
            c->status = 0;
        }
        c->track = 0;
    }
    t->chan = 0;
}

#define FETCH(t) (*(t)->cmdPtr++)
static void ply_goto_(Trk *t) { u8 *p = t->cmdPtr; t->cmdPtr = (u8 *)(u32)(p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24); }
void ply_fine(MPlay *mp, Trk *t) {
    (void)mp;
    for (Chan *c = t->chan; c; c = (Chan *)c->np) { if (c->status & CH_ACTIVE) c->status |= CH_STOP; RealClearChain(c); }
    t->flags = 0;
}
void ply_goto(MPlay *mp, Trk *t) { (void)mp; ply_goto_(t); }
void ply_patt(MPlay *mp, Trk *t) {
    if (t->patternLevel < 3) { t->patternStack[t->patternLevel] = t->cmdPtr + 4; t->patternLevel++; ply_goto_(t); }
    else ply_fine(mp, t);
}
void ply_pend(MPlay *mp, Trk *t) { (void)mp; if (t->patternLevel) { t->patternLevel--; t->cmdPtr = t->patternStack[t->patternLevel]; } }
void ply_rept(MPlay *mp, Trk *t) {
    (void)mp;
    u8 *p = t->cmdPtr;
    if (*p == 0) { t->cmdPtr = p + 1; ply_goto_(t); return; }
    t->repN++;
    t->cmdPtr = p + 1;
    if (t->repN < *p) ply_goto_(t);
    else { t->repN = 0; t->cmdPtr = p + 5; }
}
void ply_prio(MPlay *mp, Trk *t) { (void)mp; t->priority = FETCH(t); }
void ply_tempo(MPlay *mp, Trk *t) { u32 v = FETCH(t) << 1; mp->tempoD = (u16)v; mp->tempoI = (u16)((v * mp->tempoU) >> 8); }
void ply_keysh(MPlay *mp, Trk *t) { (void)mp; t->keyShift = FETCH(t); t->flags |= MPT_FLG_PITCHG; }
void ply_voice(MPlay *mp, Trk *t) {
    u32 idx = FETCH(t);
    t->tone = mp->tone[idx];
}
void ply_vol(MPlay *mp, Trk *t) { (void)mp; t->vol = FETCH(t); t->flags |= MPT_FLG_VOLCHG; }
void ply_pan(MPlay *mp, Trk *t) { (void)mp; t->pan = (s8)(FETCH(t) - C_V); t->flags |= MPT_FLG_VOLCHG; }
void ply_bend(MPlay *mp, Trk *t) { (void)mp; t->bend = (s8)(FETCH(t) - C_V); t->flags |= MPT_FLG_PITCHG; }
void ply_bendr(MPlay *mp, Trk *t) { (void)mp; t->bendRange = FETCH(t); t->flags |= MPT_FLG_PITCHG; }
void ply_lfodl(MPlay *mp, Trk *t) { (void)mp; t->lfoDelay = FETCH(t); }
void ply_modt(MPlay *mp, Trk *t) { (void)mp; u8 v = FETCH(t); if (t->modT != v) { t->modT = v; t->flags |= 0xF; } }
void ply_tune(MPlay *mp, Trk *t) { (void)mp; t->tune = (s8)(FETCH(t) - C_V); t->flags |= MPT_FLG_PITCHG; }
void ply_port(MPlay *mp, Trk *t) { (void)mp; u8 reg = FETCH(t); u8 val = FETCH(t); *(volatile u8 *)(0x04000060u + reg) = val; }
void ply_lfos(MPlay *mp, Trk *t) { (void)mp; t->lfoSpeed = FETCH(t); if (t->lfoSpeed == 0) clear_modM(t); }
void ply_mod(MPlay *mp, Trk *t) { (void)mp; t->mod = FETCH(t); if (t->mod == 0) clear_modM(t); }
void ply_endtie(MPlay *mp, Trk *t) {
    (void)mp;
    u8 key;
    if (*t->cmdPtr < 0x80) { key = *t->cmdPtr++; t->key = key; } else key = t->key;
    for (Chan *c = t->chan; c; c = (Chan *)c->np)
        if ((c->status & 0x83) && !(c->status & CH_STOP) && c->mk == key) { c->status |= CH_STOP; break; }
}

void ply_note(u32 noteIdx, MPlay *mp, Trk *t) {
    struct SoundInfo *si = SOUND_INFO_ADDR;
    t->gateTime = sClockTable[noteIdx];
    if (*t->cmdPtr < 0x80) {                                           /* optional operands: key, velocity, gate-time add-on */
        u8 *p = t->cmdPtr;
        t->key = *p++;
        if (*p < 0x80) {
            t->velocity = *p++;
            if (*p < 0x80) t->gateTime += *p++;
        }
        t->cmdPtr = p;
    }
    int pitchOff = 0;
    struct ToneData *tone = &t->tone, *voice;
    u32 key;
    if (tone->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY)) {
        u32 k = t->key, idx;
        if (tone->type & TONEDATA_TYPE_SPL) idx = (*(const u8 *const *)&tone->attack)[k];   /* 3rd word = key-split table */
        else idx = k;
        voice = (struct ToneData *)((u8 *)tone->wav + idx * 12);       /* 1st word = sub voicegroup */
        if (voice->type & (TONEDATA_TYPE_SPL | TONEDATA_TYPE_RHY)) return;
        key = k;
        if (tone->type & TONEDATA_TYPE_RHY) {
            if (voice->pan_sweep & 0x80) pitchOff = (int)((voice->pan_sweep - 0xC0) << 1);
            key = voice->key;
        }
    } else { voice = tone; key = t->key; }

    u32 prio = t->priority + mp->priority;
    if (prio > 0xFF) prio = 0xFF;
    u32 ty = voice->type & 7;
    Chan *ch;
    if (ty != 0) {                                                     /* Game Boy channel: fixed slot per type */
        ch = (Chan *)si->cgbChans;
        if (!ch) return;
        ch = (Chan *)((u8 *)ch + (ty - 1) * sizeof(struct CgbChannel));
        if ((ch->status & CH_ACTIVE) && !(ch->status & CH_STOP)) {
            if (ch->pr > prio) return;
            if (ch->pr == prio && (u32)ch->track < (u32)t) return;
        }
    } else {                                                           /* PCM: free channel, else steal the weakest one */
        Chan *best = 0; int found = 0; u32 bp = prio; u32 bt = (u32)t;
        Chan *c = si->chans;
        for (int i = 0; i < si->maxChans; i++, c++) {
            u32 st = c->status;
            if (!(st & CH_ACTIVE)) { best = c; break; }
            if (st & CH_STOP) {
                if (!found) { found = 1; bp = c->pr; bt = (u32)c->track; best = c; continue; }
            } else if (found) continue;
            if (c->pr < bp) { bp = c->pr; bt = (u32)c->track; best = c; }
            else if (c->pr == bp) {
                if ((u32)c->track > bt) { bt = (u32)c->track; best = c; }
                else if ((u32)c->track == bt) best = c;
            }
        }
        if (!best) return;
        ch = best;
    }
    /* take the channel: unlink from its old track, make it the head of this track's chain */
    ClearChain(ch);
    ch->pp = 0;
    ch->np = (u32)t->chan;
    if (t->chan) t->chan->pp = (u32)ch;
    t->chan = ch;
    ch->track = t;
    t->lfoDelayC = t->lfoDelay;
    if (t->lfoDelay != 0) clear_modM(t);
    TrkVolPitSet(mp, t);
    ch->gt = t->gateTime; ch->mk = t->key; ch->ve = t->velocity;       /* 4-byte copy in the asm; 4th byte is overwritten next */
    ch->pr = (u8)prio;
    ch->ky = (u8)key;
    ch->rp = (u8)pitchOff;
    ch->type = voice->type;
    ch->wav = voice->wav;
    ch->attack = voice->attack; ch->decay = voice->decay; ch->sustain = voice->sustain; ch->release = voice->release;
    ch->echoVolume = t->echoVolume; ch->echoLength = t->echoLength;
    ChnVolSetAsm(ch, t);
    int k = (int)ch->ky + t->keyM;
    if (k < 0) k = 0;
    if (ty != 0) {
        u8 sw = voice->pan_sweep;
        ((u8 *)ch)[0x1E] = voice->length;                              /* CgbChannel.le */
        if ((sw & 0x80) || !(sw & 0x70)) sw = 8;
        ((u8 *)ch)[0x1F] = sw;                                         /* CgbChannel.sw */
        ch->freq = si->MidiKeyToCgbFreq((u8)ty, (u8)k, t->pitM);
    } else {
        ch->freq = MidiKeyToFreq(voice->wav, (u8)k, t->pitM);
    }
    ch->status = CH_START;
    t->flags &= 0xF0;
}

/* ---- MPlayMain: one call per frame; runs as many 150-unit tempo ticks as the tempo accumulator allows ---------------- */
void MPlayMain(MPlay *mp) {
    struct SoundInfo *si = SOUND_INFO_ADDR;
    if (mp->ident != ID_NUMBER) return;
    mp->ident++;
    if (mp->func) ((void (*)(u32))mp->func)(mp->intp);                 /* next player in the chain */
    if ((s32)mp->status < 0) goto done;
    FadeOutBody(mp);
    if ((s32)mp->status < 0) goto done;
    u32 tempoC = (u32)mp->tempoC + mp->tempoI;
    for (;;) {
        mp->tempoC = (u16)tempoC;
        if (tempoC < 150) break;
        /* ---- one sequencer tick ---- */
        u32 active = 0, bit = 1;
        Trk *t = mp->tracks;
        for (int i = 0; i < mp->trackCount; i++, t++, bit <<= 1) {
            if (!(t->flags & MPT_FLG_EXIST)) continue;
            active |= bit;
            for (Chan *c = t->chan; c; c = (Chan *)c->np) {
                if (c->status & CH_ACTIVE) {
                    if (c->gt != 0 && --c->gt == 0) c->status |= CH_STOP;
                } else ClearChain(c);
            }
            if (t->flags & MPT_FLG_START) {
                Clear64byte(t);
                t->flags = MPT_FLG_EXIST;
                t->bendRange = 2; t->volX = C_V; t->lfoSpeed = 0x16; t->tone.type = 1;
            }
            while (t->wait == 0) {
                u32 cmd = *t->cmdPtr;
                if (cmd < 0x80) cmd = t->runningStatus;
                else { t->cmdPtr++; if (cmd >= 0xBD) t->runningStatus = (u8)cmd; }
                if (cmd >= 0xCF) {
                    ((void (*)(u32, MPlay *, Trk *))si->plynote)(cmd - 0xCF, mp, t);
                } else if (cmd > 0xB0) {
                    mp->cmd = (u8)(cmd - 0xB1);
                    ((MPlayFunc *)si->MPlayJumpTable)[cmd - 0xB1](mp, t);
                    if (t->flags == 0) break;                          /* track ended (ply_fine) */
                } else {
                    t->wait = sClockTable[cmd - 0x80];
                }
            }
            if (t->flags == 0) continue;
            t->wait--;
            if (t->lfoSpeed != 0 && t->mod != 0) {                     /* LFO */
                if (t->lfoDelayC != 0) { t->lfoDelayC--; continue; }
                t->lfoSpeedC += t->lfoSpeed;
                int ph = t->lfoSpeedC, w;
                if ((s8)(ph - 0x40) >= 0) w = 0x80 - ph; else w = (s8)ph;
                w = (t->mod * w) >> 6;
                if ((u8)w != (u8)t->modM) { t->modM = (s8)w; t->flags |= (t->modT == 0) ? 0xC : 3; }
            }
        }
        mp->clock++;
        if (active == 0) { mp->status = MUSICPLAYER_STATUS_PAUSE; goto done; }
        mp->status = active;
        tempoC = (u32)mp->tempoC - 150;
    }
    /* ---- apply volume / pitch changes to every playing note ---- */
    {
        Trk *t = mp->tracks;
        for (int i = 0; i < mp->trackCount; i++, t++) {
            if (!(t->flags & MPT_FLG_EXIST) || !(t->flags & 0xF)) continue;
            TrkVolPitSet(mp, t);
            for (Chan *c = t->chan; c; c = (Chan *)c->np) {
                if (!(c->status & CH_ACTIVE)) { ClearChain(c); continue; }
                u32 ty = c->type & 7;
                if (t->flags & MPT_FLG_VOLCHG) {
                    ChnVolSetAsm(c, t);
                    if (ty) ((u8 *)c)[0x1D] |= 1;
                }
                if (t->flags & MPT_FLG_PITCHG) {
                    int k = (int)c->ky + t->keyM;
                    if (k < 0) k = 0;
                    if (ty) { c->freq = si->MidiKeyToCgbFreq((u8)ty, (u8)k, t->pitM); ((u8 *)c)[0x1D] |= 2; }
                    else c->freq = MidiKeyToFreq(c->wav, (u8)k, t->pitM);
                }
            }
            t->flags &= 0xF0;
        }
    }
done:
    mp->ident = ID_NUMBER;
}
