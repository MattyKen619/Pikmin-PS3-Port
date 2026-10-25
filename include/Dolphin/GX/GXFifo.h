#ifndef _DOLPHIN_GXFIFO_H
#define _DOLPHIN_GXFIFO_H

#include "types.h"

#include "Dolphin/GX/GXEnum.h"
#include "Dolphin/GX/GXTypes.h"

BEGIN_SCOPE_EXTERN_C

/////////////// FIFO STRUCTS ///////////////
#define GX_FIFO_MINSIZE  (64 * 1024)
#define GX_FIFO_OBJ_SIZE (128)

#define GXFIFO_ADDR 0xCC008000

// Generic struct for FIFO access (size 0x80).
typedef struct _GXFifoObj {
	u8 padding[GX_FIFO_OBJ_SIZE]; // _00
} GXFifoObj;

typedef struct __GXFifoObj {
	u8* base;
	u8* top;
	u32 size;
	u32 hiWatermark;
	u32 loWatermark;
	void* rdPtr;
	void* wrPtr;
	s32 count;
	u8 bind_cpu;
	u8 bind_gp;
} __GXFifoObj;

// Internal struct for FIFO access.
typedef struct _GXFifoObjPriv {
	void* base;        // _00
	void* end;         // _04
	u32 size;          // _08
	u32 highWatermark; // _0C
	u32 lowWatermark;  // _10
	void* readPtr;     // _14
	void* writePtr;    // _18
	s32 rwDistance;    // _1C
	u8 _20[0x60];      // _20
} GXFifoObjPriv;

typedef void (*GXBreakPtCallback)(void);

#if defined(TARGET_PS3) && defined(__cplusplus)
// PS3 port: the GameCube's write-gather pipe is a hardware address; every store to it appends to the
// graphics FIFO. Here each member of GXWGFifo is an object whose assignment hands the value to the
// port's FIFO (platform/ps3/gpu/gx_fifo.cpp), in the same big-endian byte order the hardware uses.
//
// The writes go straight into the port's ring buffer while there is room up to ps3GxWriteLimit (the
// common case), and through ps3GxWrite* otherwise. Up to test build 37 every write was a call into
// gx_fifo.cpp, through a linker stub that switches TOCs, and a float went through memory to become an
// integer first; a gameplay frame writes some 80000 times. (ps3GxWriteLimit equals ps3GxWritten whenever
// the ring may not be written directly: before it exists, and while a display list is being made.)
END_SCOPE_EXTERN_C
extern "C" {
void ps3GxWrite8(unsigned int value);
void ps3GxWrite16(unsigned int value);
void ps3GxWrite32(unsigned int value);
void ps3GxWriteBytes(const void* bytes, unsigned int n);
extern unsigned char* ps3GxRing;
extern unsigned int ps3GxWritten;    // bytes written to the ring (mod 2^32)
extern unsigned int ps3GxWriteLimit; // ps3GxWritten may grow to this without the checks in gx_fifo.cpp
}
#define PS3_GX_RING_MASK ((1u << 20) - 1) // (gx_fifo.cpp: RING_SIZE)
// (Not instrumented: a write is too small to be worth a preemption point or a shadow-stack entry.)
#define PS3_GX_PIPE_WRITE __attribute__((no_instrument_function, always_inline))

PS3_GX_PIPE_WRITE static inline void ps3GxPut8(unsigned int value)
{
	const unsigned int w = ps3GxWritten;
	if (__builtin_expect(ps3GxWriteLimit - w >= 1, 1)) {
		ps3GxRing[w & PS3_GX_RING_MASK] = (unsigned char)value;
		ps3GxWritten = w + 1;
	} else {
		ps3GxWrite8(value);
	}
}

PS3_GX_PIPE_WRITE static inline void ps3GxPut16(unsigned int value)
{
	const unsigned int w = ps3GxWritten;
	if (__builtin_expect(ps3GxWriteLimit - w >= 2, 1)) {
		unsigned char* p = ps3GxRing + (w & PS3_GX_RING_MASK);
		p[0]             = (unsigned char)(value >> 8);
		p[1]             = (unsigned char)value;
		ps3GxWritten     = w + 2;
	} else {
		ps3GxWrite16(value);
	}
}

PS3_GX_PIPE_WRITE static inline void ps3GxPut32(unsigned int value)
{
	const unsigned int w = ps3GxWritten;
	if (__builtin_expect(ps3GxWriteLimit - w >= 4, 1)) {
		unsigned char* p = ps3GxRing + (w & PS3_GX_RING_MASK);
		p[0]             = (unsigned char)(value >> 24);
		p[1]             = (unsigned char)(value >> 16);
		p[2]             = (unsigned char)(value >> 8);
		p[3]             = (unsigned char)value;
		ps3GxWritten     = w + 4;
	} else {
		ps3GxWrite32(value);
	}
}

// A float: stored as it is where the ring position is word-aligned (no trip through memory to an integer
// register); bytes otherwise (the PPU faults on a floating-point store that is not word-aligned).
PS3_GX_PIPE_WRITE static inline void ps3GxPutF32(float value)
{
	const unsigned int w = ps3GxWritten;
	if (__builtin_expect(ps3GxWriteLimit - w >= 4, 1)) {
		unsigned char* p = ps3GxRing + (w & PS3_GX_RING_MASK);
		if (((unsigned long)p & 3) == 0) {
			*(float*)p = value;
		} else {
			union {
				float f;
				unsigned int u;
			} bits;
			bits.f = value;
			p[0]   = (unsigned char)(bits.u >> 24);
			p[1]   = (unsigned char)(bits.u >> 16);
			p[2]   = (unsigned char)(bits.u >> 8);
			p[3]   = (unsigned char)bits.u;
		}
		ps3GxWritten = w + 4;
	} else {
		union {
			float f;
			unsigned int u;
		} bits;
		bits.f = value;
		ps3GxWrite32(bits.u);
	}
}

// Words already in memory (a matrix's floats, say), copied as they are.
PS3_GX_PIPE_WRITE static inline void ps3GxPutWords(const void* src, unsigned int words)
{
	const unsigned int w = ps3GxWritten, n = words * 4;
	if (__builtin_expect(ps3GxWriteLimit - w >= n, 1)) {
		unsigned char* p      = ps3GxRing + (w & PS3_GX_RING_MASK);
		const unsigned int* s = (const unsigned int*)src;
		if (((unsigned long)p & 3) == 0) {
			for (unsigned int i = 0; i < words; i++) {
				((unsigned int*)p)[i] = s[i];
			}
		} else {
			for (unsigned int i = 0; i < words; i++, p += 4) {
				const unsigned int v = s[i];
				p[0]                 = (unsigned char)(v >> 24);
				p[1]                 = (unsigned char)(v >> 16);
				p[2]                 = (unsigned char)(v >> 8);
				p[3]                 = (unsigned char)v;
			}
		}
		ps3GxWritten = w + n;
	} else {
		ps3GxWriteBytes(src, n);
	}
}

// A whole register write (test build 45: GX_WRITE_RAS_REG and its kin, Dolphin/GX/GXData.h): one check for
// room and one update of the write position for the command byte and its value, the value passed in a
// register. Written as separate byte and word writes, a value read from memory (the GX library's copy of the
// register, just stored) was read again after the command byte: the compiler must assume a byte store
// through the ring's pointer may change any memory. Read straight after it was stored, that stalls the PPU.
PS3_GX_PIPE_WRITE static inline void ps3GxPutReg5(unsigned int op, unsigned int value)
{
	const unsigned int w = ps3GxWritten;
	if (__builtin_expect(ps3GxWriteLimit - w >= 5, 1)) {
		unsigned char* p = ps3GxRing + (w & PS3_GX_RING_MASK);
		p[0]             = (unsigned char)op;
		p[1]             = (unsigned char)(value >> 24);
		p[2]             = (unsigned char)(value >> 16);
		p[3]             = (unsigned char)(value >> 8);
		p[4]             = (unsigned char)value;
		ps3GxWritten     = w + 5;
	} else {
		ps3GxWrite8(op);
		ps3GxWrite32(value);
	}
}

// A CP register: command byte, register, value.
PS3_GX_PIPE_WRITE static inline void ps3GxPutReg6(unsigned int op, unsigned int reg, unsigned int value)
{
	const unsigned int w = ps3GxWritten;
	if (__builtin_expect(ps3GxWriteLimit - w >= 6, 1)) {
		unsigned char* p = ps3GxRing + (w & PS3_GX_RING_MASK);
		p[0]             = (unsigned char)op;
		p[1]             = (unsigned char)reg;
		p[2]             = (unsigned char)(value >> 24);
		p[3]             = (unsigned char)(value >> 16);
		p[4]             = (unsigned char)(value >> 8);
		p[5]             = (unsigned char)value;
		ps3GxWritten     = w + 6;
	} else {
		ps3GxWrite8(op);
		ps3GxWrite8(reg);
		ps3GxWrite32(value);
	}
}

// A BP register write (GX_WRITE_RAS_REG), test build 45: not sent if it would change nothing. The command
// processor ignores such a write (gpu_cp.cpp, loadBp) except to the registers whose writes act
// (gpu::kBpActingRegs); 40-45% of the BP writes the GX library sends are like that (the game sets every
// material's state again), 13,000 a frame in the Forest Navel's busiest moments. ps3GxBpSent holds the value
// each register was last sent to the graphics processor with, and the generation it belongs to:
// ps3GxBpUnknown starts a new one whenever the registers may have changed another way (a display list
// called, raw writes), and the slow path does when a write may go into a display list being made. A write
// right after the mask register is merged with the mask: it is sent, and not remembered.
extern "C" {
extern unsigned long long ps3GxBpSent[256];
extern unsigned int ps3GxBpGen;
extern unsigned int ps3GxBpMaskPending;
extern unsigned char ps3GxBpKeep[256]; // 1: a write that changes nothing may be left out
void ps3GxBpUnknown(void);
}

PS3_GX_PIPE_WRITE static inline void ps3GxPutBp(unsigned int word)
{
	const unsigned int reg = word >> 24;
	const unsigned int w   = ps3GxWritten;
	if (__builtin_expect(ps3GxWriteLimit - w >= 5, 1)) {
		if (__builtin_expect(ps3GxBpMaskPending == 0, 1) && ps3GxBpKeep[reg]) {
			const unsigned long long key = ((unsigned long long)word << 32) | ps3GxBpGen;
			if (ps3GxBpSent[reg] == key) {
				return;
			}
			ps3GxBpSent[reg] = key;
		} else {
			ps3GxBpMaskPending = reg == 0xFE;
			ps3GxBpSent[reg]   = 0;
		}
		unsigned char* p = ps3GxRing + (w & PS3_GX_RING_MASK);
		p[0]             = 0x61;
		p[1]             = (unsigned char)(word >> 24);
		p[2]             = (unsigned char)(word >> 16);
		p[3]             = (unsigned char)(word >> 8);
		p[4]             = (unsigned char)word;
		ps3GxWritten     = w + 5;
	} else {
		ps3GxBpGen         = ps3GxBpGen + 1;
		ps3GxBpMaskPending = reg == 0xFE;
		ps3GxWrite8(0x61);
		ps3GxWrite32(word);
	}
}

// One XF register: command byte, address word (one value), value.
PS3_GX_PIPE_WRITE static inline void ps3GxPutXfReg(unsigned int address, unsigned int value)
{
	const unsigned int w = ps3GxWritten;
	if (__builtin_expect(ps3GxWriteLimit - w >= 9, 1)) {
		unsigned char* p = ps3GxRing + (w & PS3_GX_RING_MASK);
		p[0]             = 0x10;
		p[1]             = (unsigned char)(address >> 24);
		p[2]             = (unsigned char)(address >> 16);
		p[3]             = (unsigned char)(address >> 8);
		p[4]             = (unsigned char)address;
		p[5]             = (unsigned char)(value >> 24);
		p[6]             = (unsigned char)(value >> 16);
		p[7]             = (unsigned char)(value >> 8);
		p[8]             = (unsigned char)value;
		ps3GxWritten     = w + 9;
	} else {
		ps3GxWrite8(0x10);
		ps3GxWrite32(address);
		ps3GxWrite32(value);
	}
}

struct Ps3GxPipe8 {
	PS3_GX_PIPE_WRITE void operator=(unsigned int value) volatile { ps3GxPut8(value); }
};
struct Ps3GxPipe16 {
	PS3_GX_PIPE_WRITE void operator=(unsigned int value) volatile { ps3GxPut16(value); }
};
struct Ps3GxPipe32 {
	PS3_GX_PIPE_WRITE void operator=(unsigned int value) volatile { ps3GxPut32(value); }
};
struct Ps3GxPipeF32 {
	PS3_GX_PIPE_WRITE void operator=(float value) volatile { ps3GxPutF32(value); }
};
struct PPCWGPipe {
	Ps3GxPipe8 u8;
	Ps3GxPipe8 s8;
	Ps3GxPipe16 u16;
	Ps3GxPipe16 s16;
	Ps3GxPipe32 u32;
	Ps3GxPipe32 s32;
	Ps3GxPipeF32 f32;
};
BEGIN_SCOPE_EXTERN_C
#else
// PPC Write Gather Pipe
typedef union {
	u8 u8;
	u16 u16;
	u32 u32;
	u64 u64;
	s8 s8;
	s16 s16;
	s32 s32;
	s64 s64;
	f32 f32;
	f64 f64;
} PPCWGPipe;
#endif

extern volatile PPCWGPipe GXWGFifo AT_ADDRESS(GXFIFO_ADDR);

////////////////////////////////////////////

//////////// FIFO MACROS/INLINES ///////////
// The vertex helpers below are not instrumented on the PS3 (see PS3_GX_PIPE_WRITE).
#if defined(TARGET_PS3)
#define GX_VERT_INLINE static inline __attribute__((no_instrument_function))
#else
#define GX_VERT_INLINE static inline
#endif

#define GX_WRITE_U8(val)  (GXWGFifo.u8 = val)
#define GX_WRITE_U16(val) (GXWGFifo.u16 = val)
#define GX_WRITE_S16(val) (GXWGFifo.s16 = val)
#define GX_WRITE_U32(val) (GXWGFifo.u32 = (u32)val)
#define GX_WRITE_F32(val) (GXWGFifo.f32 = (f32)val)

GX_VERT_INLINE void GXPosition2f32(const f32 x, const f32 y)
{
	GXWGFifo.f32 = x;
	GXWGFifo.f32 = y;
}

GX_VERT_INLINE void GXPosition3s16(const s16 x, const s16 y, const s16 z)
{
	GXWGFifo.s16 = x;
	GXWGFifo.s16 = y;
	GXWGFifo.s16 = z;
}

GX_VERT_INLINE void GXPosition3u16(const u16 x, const u16 y, const u16 z)
{
	GXWGFifo.u16 = x;
	GXWGFifo.u16 = y;
	GXWGFifo.u16 = z;
}

GX_VERT_INLINE void GXPosition3f32(f32 x, f32 y, f32 z)
{
	GXWGFifo.f32 = x;
	GXWGFifo.f32 = y;
	GXWGFifo.f32 = z;
}

GX_VERT_INLINE void GXNormal3f32(const f32 x, const f32 y, const f32 z)
{
	GXWGFifo.f32 = x;
	GXWGFifo.f32 = y;
	GXWGFifo.f32 = z;
}

GX_VERT_INLINE void GXColor1u32(u32 c)
{
	GXWGFifo.u32 = c;
}

GX_VERT_INLINE void GXColor4u8(const u8 r, const u8 g, const u8 b, const u8 a)
{
	GXWGFifo.u8 = r;
	GXWGFifo.u8 = g;
	GXWGFifo.u8 = b;
	GXWGFifo.u8 = a;
}

GX_VERT_INLINE void GXTexCoord2s8(const s8 u, const s8 v)
{
	GXWGFifo.s8 = u;
	GXWGFifo.s8 = v;
}

GX_VERT_INLINE void GXTexCoord2u8(u8 s, u8 t)
{
	GXWGFifo.u8 = s;
	GXWGFifo.u8 = t;
}

GX_VERT_INLINE void GXPosition2u16(u16 x, u16 y)
{
	GXWGFifo.u16 = x;
	GXWGFifo.u16 = y;
}

GX_VERT_INLINE void GXPosition2s16(s16 x, s16 y)
{
	GXWGFifo.s16 = x;
	GXWGFifo.s16 = y;
}

GX_VERT_INLINE void GXTexCoord2s16(const s16 u, const s16 v)
{
	GXWGFifo.s16 = u;
	GXWGFifo.s16 = v;
}

GX_VERT_INLINE void GXTexCoord2u16(const u16 u, const u16 v)
{
	GXWGFifo.u16 = u;
	GXWGFifo.u16 = v;
}

GX_VERT_INLINE void GXTexCoord2f32(const f32 u, const f32 v)
{
	GXWGFifo.f32 = u;
	GXWGFifo.f32 = v;
}

GX_VERT_INLINE void GXEnd(void)
{
}

////////////////////////////////////////////

//////////// FIFO INIT/SET/SAVE ////////////
// Init.
extern void __GXFifoInit();
extern void GXInitFifoBase(GXFifoObj* obj, void* base, u32 size);
extern void GXInitFifoPtrs(GXFifoObj* obj, void* readPtr, void* writePtr);
extern void GXInitFifoLimits(GXFifoObj* obj, u32 hiWaterMark, u32 loWaterMark);

// Set.
extern void GXSetCPUFifo(GXFifoObj* obj);
extern void GXSetGPFifo(GXFifoObj* obj);
extern void GXSaveCPUFifo(GXFifoObj* obj);

////////////////////////////////////////////

/////////////// FIFO GETTERS ///////////////
extern void GXGetGPStatus(GXBool* isOverHi, GXBool* isUnderLo, GXBool* isReadIdle, GXBool* isCmdIdle, GXBool* isHitBrkPt);
extern GXFifoObj* GXGetCPUFifo();
extern GXFifoObj* GXGetGPFifo();

////////////////////////////////////////////

//////////// DISPLAY LIST FUNCS ////////////
extern void GXBeginDisplayList(void* list, u32 size);
extern u32 GXEndDisplayList();
extern void GXCallDisplayList(void* list, u32 numBytes);

////////////////////////////////////////////

///////////// BREAKPOINT FUNCS /////////////
extern GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback callback);

////////////////////////////////////////////

/////////////// OTHER FUNCS ////////////////
void __GXSaveCPUFifoAux(__GXFifoObj* obj);
void __GXFifoReadEnable();
void __GXFifoReadDisable();
void __GXFifoLink(u8);
void __GXWriteFifoIntEnable(u8, u8);
void __GXWriteFifoIntReset(u8, u8);
void __GXCleanGPFifo(void);

// Unused/inlined in P2.
extern void GXSaveGPFifo(GXFifoObj* obj);

extern void GXGetFifoStatus(GXFifoObj* obj, GXBool* isOverHi, GXBool* isUnderLo, u32* fifoCount, GXBool* isCpuWrite, GXBool* isGPRead,
                            GXBool* isFifoWrap);
extern void GXGetFifoPtrs(GXFifoObj* obj, void** readPtr, void** writePtr);
extern void* GXGetFifoBase(GXFifoObj* obj);
extern u32 GXGetFifoSize(GXFifoObj* obj);
extern void GXGetFifoLimits(GXFifoObj* obj, u32* hi, u32* lo);

extern void GXEnableBreakPt(void* breakPtr);
extern void GXDisableBreakPt();

////////////////////////////////////////////

END_SCOPE_EXTERN_C

#endif
