// PS3 port: the GameCube graphics hardware as Nintendo's GX library (src/gx) sees it.
//
// The library writes its commands to the write-gather pipe (GXWGFifo, a C++ object on the PS3, see
// Dolphin/GX/GXFifo.h): ps3GxWrite8/16/32 below. When the CPU FIFO is the graphics processor's FIFO
// (always, except while a display list is recorded), the bytes go straight to the command processor
// (gpu_cp.cpp), which executes them as they arrive. While a display list is recorded, they are stored in
// the list's buffer, exactly as the hardware would, and the processor reads the list when it is called.
//
// The registers the library touches directly (__piReg, __cpReg, __peReg, __memReg) are arrays here. The
// processor raises the pixel engine's interrupts (draw done, tokens), so GXDrawDone and GXReadDrawSync
// work as on the GameCube.
//
// Nothing is drawn yet (Phase 4 step 1): the processor checks and counts every command, and records
// selected frames to the USB drive (gx_trace.cpp) for checking off the console.

#include "../sdk/sdk_internal.h"

#include "../backend/ps3_backend.h"

#include "Dolphin/GX/GXFifo.h"
#include "Dolphin/OS/OSInterrupt.h"
#include "Dolphin/hw_regs.h"
#include "gx_internal.h"
#include "ps3/gxport.h"
#include <stdio.h>
#include <string.h>

extern "C" void ps3RaiseInterrupt(int interrupt);

using gpu::MemKind;

// ================================================================== hardware registers

static vu32 sPiRegs[0x40];
static vu16 sCpRegs[0x80];
static vu16 sPeRegs[0x40];
static vu16 sMemRegs[0x80];

extern "C" vu32* ps3GxPiRegs(void) { return sPiRegs; }
extern "C" vu16* ps3GxCpRegs(void)
{
	sCpRegs[CP_STATUS] = 0x0C; // GP read and command processing idle
	return sCpRegs;
}
extern "C" vu16* ps3GxPeRegs(void) { return sPeRegs; }
extern "C" vu16* ps3GxMemRegs(void) { return sMemRegs; }

// ================================================================== game memory as the GPU sees it
//
// Commands carry addresses with only some of their bits: display list calls keep all 32, the vertex
// array registers 30, texture, TLUT and copy addresses 26 (21 bits of address >> 5). The game's memory is
// the OS arena (96 MB, above 1 GB on the PS3) plus the executable's data. 30 bits identify an arena
// address uniquely; 26 bits do not (the arena is larger than 64 MB), so the GX library tells the port
// every texture and copy address it is given (ps3GxNoteAddress), and that table resolves them.

static uptr sArenaLo, sArenaHi;
static const uptr kLowLo = 0x10000, kLowHi = 0x10000000; // executable and static data

enum { BOOK_SIZE = 1 << 14 };
static u32 sBookKey[BOOK_SIZE]; // (address >> 5) & 0x1FFFFF, + 1 (0 = empty)
static u32 sBookPtr[BOOK_SIZE];
static u32 sBookCount;

extern "C" void ps3GxNoteAddress(const void* ptr)
{
	u32 p   = (u32)(uptr)ptr;
	u32 key = ((p >> 5) & 0x1FFFFF) + 1;
	if (sBookCount >= BOOK_SIZE * 3 / 4) {
		memset(sBookKey, 0, sizeof(sBookKey)); // full: start over (old entries are rarely still in use)
		sBookCount = 0;
	}
	u32 h = (key * 2654435761u) & (BOOK_SIZE - 1);
	while (sBookKey[h] && sBookKey[h] != key) {
		h = (h + 1) & (BOOK_SIZE - 1);
	}
	if (!sBookKey[h]) {
		sBookKey[h] = key;
		sBookCount++;
	}
	sBookPtr[h] = p;
}

static u32 bookLookup(u32 address26)
{
	u32 key = ((address26 >> 5) & 0x1FFFFF) + 1;
	u32 h   = (key * 2654435761u) & (BOOK_SIZE - 1);
	while (sBookKey[h]) {
		if (sBookKey[h] == key) {
			return sBookPtr[h];
		}
		h = (h + 1) & (BOOK_SIZE - 1);
	}
	return 0;
}

static const gpu::u8* inRegion(uptr p, u32 size)
{
	if (sArenaLo && p >= sArenaLo && p + size <= sArenaHi) {
		return (const gpu::u8*)p;
	}
	if (p >= kLowLo && p + size <= kLowHi) {
		return (const gpu::u8*)p;
	}
	return NULL;
}

static u32 sAmbiguous;

static const gpu::u8* translate(u32 address, u32 size, MemKind kind)
{
	switch (kind) {
	case gpu::MEM_DISPLAY_LIST:
		return inRegion(address, size);
	case gpu::MEM_ARRAY:
	{
		u32 a30 = address & 0x3FFFFFFF;
		if (sArenaLo) {
			const gpu::u8* p = inRegion((sArenaLo & ~(uptr)0x3FFFFFFF) | a30, size);
			if (p) {
				return p;
			}
		}
		return inRegion(a30, size);
	}
	default:
	{
		u32 a26 = address & 0x3FFFFFF;
		u32 full = bookLookup(a26);
		if (full && (full & 0x3FFFFFF) == a26) {
			const gpu::u8* p = inRegion(full, size);
			if (p) {
				return p;
			}
		}
		if (sArenaLo) {
			uptr c = (sArenaLo & ~(uptr)0x3FFFFFF) | a26;
			if (c < sArenaLo) {
				c += 0x4000000;
			}
			const gpu::u8* p = inRegion(c, size);
			if (p) {
				if (c + 0x4000000 + size <= sArenaHi) {
					sAmbiguous++;
				}
				return p;
			}
		}
		return inRegion(a26, size);
	}
	}
}

class GameMemory : public gpu::Memory {
public:
	const gpu::u8* read(gpu::u32 address, gpu::u32 size, MemKind kind) override
	{
		const gpu::u8* p = translate(address, size, kind);
		if (p && ps3gx::traceRecording()) {
			ps3gx::traceMemory(address, size, kind, p);
		}
		return p;
	}
};

// ================================================================== statistics and events

static gpu::CommandProcessor sCp;
static GameMemory sMemory;
static gpu::Listener* sRenderer;

gpu::CommandProcessor& ps3gx::commandProcessor() { return sCp; }
void ps3gx::setRenderer(gpu::Listener* renderer) { sRenderer = renderer; }

namespace {

// Small set of 64-bit hashes (distinct TEV setups, vertex formats).
struct HashSet {
	enum { SIZE = 2048 };
	gpu::u64 slot[SIZE];
	u32 count;
	bool insert(gpu::u64 h)
	{
		if (h == 0) {
			h = 1;
		}
		u32 i = (u32)(h ^ (h >> 32)) & (SIZE - 1);
		while (slot[i]) {
			if (slot[i] == h) {
				return false;
			}
			i = (i + 1) & (SIZE - 1);
		}
		if (count >= SIZE * 3 / 4) {
			return false;
		}
		slot[i] = h;
		count++;
		return true;
	}
};

inline gpu::u64 fnv(gpu::u64 h, u32 v)
{
	for (int i = 0; i < 4; i++) {
		h = (h ^ ((v >> (i * 8)) & 0xFF)) * 1099511628211ULL;
	}
	return h;
}

} // namespace

static HashSet sTevSetups;
static HashSet sVertexFormats;
static u32 sTextureFormats;   // bit per texture format used
static u32 sMaxTextureSize;   // largest texture (bytes)
static u32 sMaxDrawVertices;
static volatile u32 sFrames;
static gpu::u64 sFrameDraws0, sFrameVerts0, sFrameDls0;
static u32 sLastDraws, sLastVerts, sLastDls;
static u32 sTextureReads, sTextureFailures;
static unsigned long long sCpTime;        // timebase ticks spent interpreting commands
static unsigned long long sCpTimeReported;
static u32 sFramesReported;

extern "C" unsigned int ps3FramesDrawn(void) { return sFrames; }

class Events : public gpu::Listener {
public:
	void draw(const gpu::DrawCall& call) override
	{
		const gpu::State& S = sCp.state();
		if (call.count > sMaxDrawVertices) {
			sMaxDrawVertices = call.count;
		}

		// vertex format
		gpu::u64 h = 14695981039346656037ULL;
		h          = fnv(h, S.cp[0x50]);
		h          = fnv(h, S.cp[0x60]);
		h          = fnv(h, S.cp[0x70 + call.vat]);
		h          = fnv(h, S.cp[0x80 + call.vat]);
		h          = fnv(h, S.cp[0x90 + call.vat]);
		sVertexFormats.insert(h);

		// TEV setup: stage count, texgen/color counts, color and alpha combiners, orders, constant
		// selections, alpha test
		const u32 genmode = S.bp[0x00];
		const u32 stages  = ((genmode >> 10) & 15) + 1;
		h                 = 14695981039346656037ULL;
		h                 = fnv(h, genmode & 0x3C7F);
		for (u32 s = 0; s < stages; s++) {
			h = fnv(h, S.bp[0xC0 + s * 2]);
			h = fnv(h, S.bp[0xC1 + s * 2]);
		}
		for (u32 s = 0; s < (stages + 1) / 2; s++) {
			h = fnv(h, S.bp[0x28 + s]);
			h = fnv(h, S.bp[0xF6 + s]);
		}
		h = fnv(h, S.bp[0xF3]);
		sTevSetups.insert(h);

		// textures used by the stages
		u32 maps = 0;
		for (u32 s = 0; s < stages; s++) {
			u32 tref = S.bp[0x28 + s / 2] >> ((s & 1) * 12);
			if (tref & 0x40) {
				maps |= 1u << (tref & 7);
			}
		}
		for (u32 m = 0; m < 8; m++) {
			if (!(maps & (1u << m))) {
				continue;
			}
			u32 address, width, height, format, levels;
			sCp.textureInfo(m, address, width, height, format, levels);
			u32 size = gpu::CommandProcessor::textureSize(format, width, height, levels);
			sTextureFormats |= 1u << (format & 15);
			if (size > sMaxTextureSize) {
				sMaxTextureSize = size;
			}
			sTextureReads++;
			if (!sMemory.read(address, size, gpu::MEM_TEXTURE)) {
				sTextureFailures++;
				memoryFailed(address, size, gpu::MEM_TEXTURE);
			}
		}
		if (sRenderer) {
			sRenderer->draw(call);
		}
	}

	void copy(const gpu::CopyInfo& info) override
	{
		if (sRenderer) {
			sRenderer->copy(info);
		}
		if (!info.toXfb) {
			return;
		}
		// A finished frame.
		gpu::Stats& st = sCp.stats();
		sLastDraws     = (u32)(st.draws - sFrameDraws0);
		sLastVerts     = (u32)(st.vertices - sFrameVerts0);
		sLastDls       = (u32)(st.displayLists - sFrameDls0);
		sFrameDraws0   = st.draws;
		sFrameVerts0   = st.vertices;
		sFrameDls0     = st.displayLists;
		sFrames++;
		if (sFrames == 1) {
			ps3Log("[gpu] first frame: %u draws, %u vertices, %u display lists\n", sLastDraws, sLastVerts, sLastDls);
		}
		ps3NoteActivity("GX frame");
		ps3gx::traceFrameEnd(sFrames, sCp);
	}

	void drawDone() override { ps3RaiseInterrupt(__OS_INTERRUPT_PI_PE_FINISH); }

	void token(gpu::u16 value, bool interrupt) override
	{
		sPeRegs[PE_TOKEN] = value;
		if (interrupt) {
			ps3RaiseInterrupt(__OS_INTERRUPT_PI_PE_TOKEN);
		}
	}

	void badCommand(gpu::u8 opcode, const gpu::u8* where, gpu::u32 available, bool inDisplayList) override
	{
		static int sLogged;
		if (sLogged < 5) {
			sLogged++;
			char bytes[64];
			int n = 0;
			for (u32 i = 0; i < available && i < 12; i++) {
				n += snprintf(bytes + n, sizeof(bytes) - n, " %02x", where[i]);
			}
			ps3Log("[gpu] unknown or incomplete command 0x%02x%s:%s\n", opcode, inDisplayList ? " in a display list" : "", bytes);
		}
	}

	void memoryFailed(gpu::u32 address, gpu::u32 size, MemKind kind) override
	{
		static int sLogged;
		static const char* const kKinds[] = { "display list", "vertex array", "texture", "TLUT" };
		if (sLogged < 8) {
			sLogged++;
			ps3Log("[gpu] %s at 0x%08x (%u bytes) is outside game memory\n", kKinds[kind < 4 ? kind : 0], (unsigned)address,
			       (unsigned)size);
		}
	}
};

static Events sEvents;

// ================================================================== the CPU FIFO

static __GXFifoObj* sCpuFifo;
static bool sLinked;
static bool sStarted;

static void start()
{
	sStarted = true;
	sArenaLo = (uptr)OSGetArenaLo();
	sArenaHi = (uptr)OSGetArenaHi();
	sCp.setMemory(&sMemory);
	sCp.setListener(&sEvents);
	ps3Log("[gpu] graphics commands are interpreted (not drawn yet); game memory 0x%08x-0x%08x\n", (unsigned)sArenaLo,
	       (unsigned)sArenaHi);
}

static void flushStage();

extern "C" void ps3GxSetCpuFifo(__GXFifoObj* fifo, int linkedToGp)
{
	if (!sStarted) {
		start();
	}
	flushStage(); // what was written so far goes to the graphics processor first
	sCpuFifo              = fifo;
	sLinked               = linkedToGp != 0;
	sPiRegs[PI_FIFO_PTR]  = 0; // clears the overflow (wrap) bit GXEndDisplayList checks
}

extern "C" void ps3GxSaveCpuFifo(__GXFifoObj* fifo)
{
	if (fifo == sCpuFifo && !sLinked) {
		// The hardware writes in 32-byte bursts: pad the stored commands with NOPs to the next boundary.
		u8* w   = (u8*)fifo->wrPtr;
		u8* end = fifo->base + fifo->size;
		while (((uptr)w & 31) && w < end) {
			*w++ = 0;
		}
		fifo->wrPtr = w;
		fifo->count = (s32)(w - (u8*)fifo->rdPtr);
	} else {
		fifo->rdPtr = fifo->base;
		fifo->wrPtr = fifo->base;
		fifo->count = 0;
	}
}

static void store(const u8* bytes, u32 n)
{
	if (!sCpuFifo) {
		return;
	}
	u8* w   = (u8*)sCpuFifo->wrPtr;
	u8* end = sCpuFifo->base + sCpuFifo->size;
	if (w + n > end) {
		sPiRegs[PI_FIFO_PTR] |= 1u << 26; // overflow: GXEndDisplayList reports it
		static int sLogged;
		ps3LogOnce(&sLogged, "[gpu] a display list overflowed its buffer (%u bytes)\n", (unsigned)sCpuFifo->size);
		return;
	}
	memcpy(w, bytes, n);
	sCpuFifo->wrPtr = w + n;
}

// Commands written while the FIFOs are linked are collected here and interpreted in batches: when the
// buffer is full, at GXFlush (which the library calls before it waits for the graphics processor, e.g. in
// GXDrawDone), and before the CPU FIFO changes. The GameCube's graphics processor also runs behind the
// CPU, so the game never relies on a command taking effect sooner.
enum { STAGE_SIZE = 16 * 1024 }; // small enough that interpreting a batch does not hold up the audio thread
static u8 sStage[STAGE_SIZE];
static u32 sStageLen;

static void flushStage()
{
	if (sStageLen == 0) {
		return;
	}
	ps3gx::traceAtFlush(sCp);
	if (ps3gx::traceRecording()) {
		ps3gx::traceFifo(sStage, sStageLen);
	}
	unsigned long long t0 = ps3b_timebase();
	u32 len               = sStageLen;
	sStageLen             = 0; // (interpreting can end a frame, which may start a trace)
	sCp.write(sStage, len);
	sCpTime += ps3b_timebase() - t0;
}

extern "C" void ps3GxFlush(void) { flushStage(); }

static inline void stage(const u8* b, u32 n)
{
	if (sStageLen + n > STAGE_SIZE) {
		flushStage();
	}
	for (u32 i = 0; i < n; i++) {
		sStage[sStageLen + i] = b[i];
	}
	sStageLen += n;
}

extern "C" void ps3GxWrite8(unsigned int value)
{
	u8 b = (u8)value;
	if (sLinked) {
		stage(&b, 1);
	} else {
		store(&b, 1);
	}
}

extern "C" void ps3GxWrite16(unsigned int value)
{
	u8 b[2] = { (u8)(value >> 8), (u8)value };
	if (sLinked) {
		stage(b, 2);
	} else {
		store(b, 2);
	}
}

extern "C" void ps3GxWrite32(unsigned int value)
{
	u8 b[4] = { (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value };
	if (sLinked) {
		stage(b, 4);
	} else {
		store(b, 4);
	}
}

// ================================================================== status (heartbeat)

extern "C" void ps3GxStatus(char* buf, unsigned int size)
{
	const gpu::Stats& st = sCp.stats();
	static const char* const kFormats[16] = { "I4", "I8", "IA4", "IA8", "RGB565", "RGB5A3", "RGBA8", "?7",
		                                      "C4", "C8", "C14X2", "?11", "?12", "?13", "CMPR", "?15" };
	char formats[96];
	int n      = 0;
	formats[0] = 0;
	for (int i = 0; i < 16; i++) {
		if (sTextureFormats & (1u << i)) {
			n += snprintf(formats + n, sizeof(formats) - n, "%s%s", n ? " " : "", kFormats[i]);
		}
	}
	char trace[96];
	ps3gx::traceStatus(trace, sizeof(trace));
	// Time spent interpreting commands, per frame, since the last report.
	u32 frames = sFrames - sFramesReported;
	unsigned long long ticks = sCpTime - sCpTimeReported;
	sFramesReported          = sFrames;
	sCpTimeReported          = sCpTime;
	unsigned long long perFrameUs = frames ? ticks * 1000000ULL / ps3b_timebase_freq() / frames : 0;
	snprintf(buf, size,
	         "graphics: %u frames (last: %u draws, %u vertices, %u display lists; %u.%u ms per frame interpreting commands); "
	         "%u TEV setups, %u vertex formats, textures %s (largest %u KB); %llu bad commands, %llu memory failures "
	         "(%u textures), %u ambiguous addresses; %s",
	         (unsigned)sFrames, sLastDraws, sLastVerts, sLastDls, (unsigned)(perFrameUs / 1000), (unsigned)(perFrameUs % 1000 / 100),
	         sTevSetups.count, sVertexFormats.count,
	         formats[0] ? formats : "none", sMaxTextureSize >> 10, (unsigned long long)st.badCommands,
	         (unsigned long long)st.memoryFailures, sTextureFailures, sAmbiguous, trace);
}
