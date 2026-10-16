#ifndef _PS3_GXPORT_H
#define _PS3_GXPORT_H

// PS3 port: what Nintendo's GX library (src/gx, TARGET_PS3 parts) needs from the port's graphics
// layer (platform/ps3/gpu). The GameCube's graphics hardware registers become plain arrays, and the
// command FIFO is handled by platform/ps3/gpu/gx_fifo.cpp.

#include "types.h"

BEGIN_SCOPE_EXTERN_C

struct __GXFifoObj;

// Register blocks the library reads and writes directly (__piReg, __cpReg, __peReg, __memReg).
vu32* ps3GxPiRegs(void);
vu16* ps3GxCpRegs(void);
vu16* ps3GxPeRegs(void);
vu16* ps3GxMemRegs(void);

// The FIFO the CPU writes to. linkedToGp: it is also the FIFO the graphics processor reads, so commands
// are executed as they are written; otherwise (a display list being recorded) they are stored.
void ps3GxSetCpuFifo(struct __GXFifoObj* fifo, int linkedToGp);
// Updates fifo's write pointer and count from what has been written (GXSaveCPUFifo, GXEndDisplayList).
void ps3GxSaveCpuFifo(struct __GXFifoObj* fifo);
// GXFlush: commands are collected and interpreted in batches; this interprets what is collected (as the
// GameCube's graphics processor would have caught up by the time the CPU waits for it).
void ps3GxFlush(void);

// Every texture, TLUT and copy address the library is given: commands keep only 26 bits of them, and the
// port needs the full address to find the data (the game's memory is larger than 64 MB on the PS3).
void ps3GxNoteAddress(const void* ptr);

END_SCOPE_EXTERN_C

#endif
