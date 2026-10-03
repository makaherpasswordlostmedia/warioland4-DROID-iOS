// port override of src/sram.c (no self-modifying code)
#include "sram.h"

typedef void* (*SramOperationFunc)(u8*, u8*, u32);

const char sSramVersion[] = "SRAM_V112";

void SramWriteUncheckedInternal(u8* src, u8* dest, u32 size)
{
    while (size-- != 0) {
        *dest++ = *src++;
    }
}

void SramWriteUnchecked(u8* src, u8* dest, u32 size)
{
    /* port: the original copies the routine to the stack and jumps into it (SRAM is only
       reachable from RAM code on hardware).  A wasm module cannot execute data. */
    SramWriteUncheckedInternal(src, dest, size);
}

void SramWrite(u8* src, u8* dest, u32 size)
{
    REG_WAITCNT = (REG_WAITCNT & ~WAITCNT_SRAM_MASK) | WAITCNT_SRAM_8;

    while (size-- != 0) {
        *dest++ = *src++;
    }
}

u8* SramCheckInternal(u8* src, u8* dest, u32 size)
{
    while (size-- != 0) {
        if (*dest++ != *src++) {
            return dest - 1;
        }
    }

    return NULL;
}

u8* SramCheck(u8* src, u8* dest, u32 size)
{
    return SramCheckInternal(src, dest, size);
}

u8* SramWriteChecked(u8* src, u8* dest, u32 size)
{
    u8* diff;
    u8 i;

    for (i = 0; i < 3; i++) {
        SramWrite(src, dest, size);
        diff = SramCheck(src, dest, size);
        if (!diff) {
            break;
        }
    }

    return diff;
}
