// PS3 port: the GameCube graphics hardware as Nintendo's GX library (src/gx) sees it.
//
// The library writes its commands to the write-gather pipe (GXWGFifo, a C++ object on the PS3, see
// Dolphin/GX/GXFifo.h): ps3GxWrite8/16/32 below. When the CPU FIFO is the graphics processor's FIFO
// (always, except while a display list is recorded), the bytes go into a ring buffer, and the command
// processor (gpu_cp.cpp) executes them on its own thread ("the graphics thread"), on the PPU's second
// hardware thread, as the GameCube's graphics chip ran beside its CPU. While a display list is recorded,
// the bytes are stored in the list's buffer, exactly as the hardware would, and the processor reads the
// list when it is called.
//
// The registers the library touches directly (__piReg, __cpReg, __peReg, __memReg) are arrays here. The
// processor raises the pixel engine's interrupts (draw done, tokens), so GXDrawDone and GXReadDrawSync
// work as on the GameCube.
//
// The processor's draws and copies go to the RSX renderer (platform/ps3/rsx), which takes the display over
// from the text console. The processor also checks and counts every command, and records selected frames
// to the USB drive (gx_trace.cpp) for checking off the console, with the RSX's picture of each recorded
// frame. If PIKMIN/gx_sync.txt exists on the USB drive, commands are interpreted on the game's thread
// instead (as in test builds 17 and 18).

#include "../sdk/sdk_internal.h"

#include "../backend/ps3_backend.h"

#include "Dolphin/GX/GXFifo.h"
#include "Dolphin/OS/OSInterrupt.h"
#include "Dolphin/hw_regs.h"
#include "../rsx/rsx_port.h"
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

// ================================================================== locks between the game and the graphics thread

void ps3gx::spinLock(volatile int* lock)
{
	int spins = 0;
	while (__sync_lock_test_and_set(lock, 1)) {
		while (*lock) {
			if (++spins > 2000) {
				ps3b_sleep_us(1); // (the holder may have been preempted)
				spins = 0;
			}
		}
	}
}

void ps3gx::spinUnlock(volatile int* lock) { __sync_lock_release(lock); }

// ================================================================== game memory as the GPU sees it
//
// Commands carry addresses with only some of their bits: display list calls keep all 32, the vertex
// array registers 30, texture, TLUT and copy addresses 26 (21 bits of address >> 5). The game's memory is
// the OS arena (96 MB, above 1 GB on the PS3) plus the executable (code and data below 512 MB). 30 bits
// identify an arena address uniquely; 26 bits do not (the arena is larger than 64 MB), so the GX library
// tells the port every texture and copy address it is given (ps3GxNoteAddress), and that table resolves
// them.

static uptr sArenaLo, sArenaHi;
// The executable: code and descriptors from 0x10000, read-only data from 0x10000000, data and bss
// after it (test 18: a vertex array in .data at 0x100b5100).
static const uptr kLowLo = 0x10000, kLowHi = 0x20000000;

enum { BOOK_SIZE = 1 << 16 };
static u32 sBookKey[BOOK_SIZE]; // (address >> 5) & 0x1FFFFF, + 1 (0 = empty)
static u32 sBookPtr[BOOK_SIZE];
static u32 sBookCount;
static u32 sBookResets;
static volatile int sBookLock;

extern "C" void ps3GxNoteAddress(const void* ptr)
{
	u32 p   = (u32)(uptr)ptr;
	u32 key = ((p >> 5) & 0x1FFFFF) + 1;
	ps3gx::spinLock(&sBookLock);
	if (sBookCount >= BOOK_SIZE * 3 / 4) {
		memset(sBookKey, 0, sizeof(sBookKey)); // full: start over (old entries are rarely still in use)
		sBookCount = 0;
		sBookResets++;
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
	ps3gx::spinUnlock(&sBookLock);
}

static u32 bookLookup(u32 address26)
{
	u32 key = ((address26 >> 5) & 0x1FFFFF) + 1;
	u32 h   = (key * 2654435761u) & (BOOK_SIZE - 1);
	u32 found = 0;
	ps3gx::spinLock(&sBookLock);
	while (sBookKey[h]) {
		if (sBookKey[h] == key) {
			found = sBookPtr[h];
			break;
		}
		h = (h + 1) & (BOOK_SIZE - 1);
	}
	ps3gx::spinUnlock(&sBookLock);
	return found;
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

static u32 sAmbiguous; // texture addresses not in the book that could be either of two arena addresses
static u32 sUnnoted;   // texture addresses not in the book

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
		if (a26 == 0) {
			return NULL; // a texture map that was never loaded
		}
		sUnnoted++;
		if (sUnnoted <= 5) {
			static const char* const kKinds[] = { "display list", "vertex array", "texture", "TLUT" };
			ps3Log("[gpu] %s address 0x%07x (%u bytes) was not given to GXInitTexObj/GXInitTlutObj/GXCopyTex%s\n",
			       kKinds[kind < 4 ? kind : 0], (unsigned)a26, (unsigned)size, sBookResets ? " (or the address table was reset)" : "");
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
			gpu::PhaseScope checks(gpu::GFX_CHECKS);
			uptr q = (uptr)p;
			if (sArenaLo && q >= sArenaLo && q < sArenaHi) {
				ps3gx::traceMemory(address, size, kind, p, (const gpu::u8*)sArenaLo, (const gpu::u8*)sArenaHi);
			} else {
				ps3gx::traceMemory(address, size, kind, p, (const gpu::u8*)kLowLo, (const gpu::u8*)kLowHi);
			}
		}
		return p;
	}
};

// ================================================================== statistics and events

static gpu::CommandProcessor sCp;
static GameMemory sMemory;
static bool sStarted;
static gpu::Listener* sRenderer;
static bool sRendererIsRsx;

gpu::CommandProcessor& ps3gx::commandProcessor() { return sCp; }
void ps3gx::setRenderer(gpu::Listener* renderer)
{
	sRenderer = renderer;
	if (sStarted) {
		sCp.setRegisterListener(renderer);
	}
}

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

// The detailed checks (vertex arrays and textures in game memory, distinct TEV setups and vertex
// formats) cost time on every draw, so they run on one frame in CHECK_EVERY, and on recorded frames. (Test
// builds up to 31 checked one frame in 30: in gameplay that frame ran late, about 29 frames a second. Up to
// test build 39, one frame a minute: a hitch each time.)
enum { CHECK_EVERY = 9000 };
static bool sCheckFrame = true;

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
static unsigned long long sReportedAt;
static u32 sFramesReported;

extern "C" unsigned int ps3FramesDrawn(void) { return sFrames; }

class Events : public gpu::Listener {
public:
	// On check frames and recorded frames every draw's vertex arrays are read (checked, and recorded with the
	// frame): by the command processor, or with draw runs, part by part in draw.
	gpu::VertexDetail vertexDetail() override
	{
		if (sRenderer) {
			gpu::VertexDetail d = sRenderer->vertexDetail();
			return d == gpu::VERTICES_NONE && !sRenderer->drawRuns() && (sCheckFrame || ps3gx::traceRecording()) ? gpu::VERTICES_ARRAYS : d;
		}
		return (sCheckFrame || ps3gx::traceRecording()) ? gpu::VERTICES_ARRAYS : gpu::VERTICES_NONE;
	}

	bool drawRuns() override { return sRenderer && sRenderer->drawRuns(); }

	void draw(const gpu::DrawCall& call) override
	{
		if (sCheckFrame || ps3gx::traceRecording()) {
			gpu::PhaseScope checks(gpu::GFX_CHECKS);
			if (call.arrays || call.vertices) {
				check(call);
			} else {
				gpu::forEachDrawPart(call, [&](const gpu::DrawCall& c) {
					sCp.resolveArrays(c);
					check(c);
				});
			}
		}
		if (sRenderer) {
			sRenderer->draw(call);
		}
	}

	void check(const gpu::DrawCall& call)
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
	}

	// (Register loads go to the renderer directly: CommandProcessor::setRegisterListener in start.)

	void copy(const gpu::CopyInfo& info) override
	{
		if (sRenderer) {
			if (info.toXfb && sRendererIsRsx && ps3gx::traceRecording()) {
				// (the recorded frame ends here: keep the RSX's picture of it too)
				char name[64], file[80];
				if (ps3gx::traceCurrentName(name, sizeof(name))) {
					snprintf(file, sizeof(file), "rsx_frame_%s", name);
					ps3rsx::captureFrame(info, file);
				}
			}
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
		sCheckFrame = (sFrames % CHECK_EVERY) == 0;
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

// ================================================================== the ring buffer and the graphics thread
//
// The game thread appends to the ring (ps3GxWritten) and publishes what it wrote (sPublished) every
// PUBLISH_CHUNK bytes and at GXFlush. The graphics thread interprets published bytes (sConsumed).
// Counters run freely (mod 2^32); the ring index is counter & RING_MASK.

enum {
	RING_SIZE     = 1 << 20,
	RING_MASK     = RING_SIZE - 1,
	PUBLISH_CHUNK = 16 * 1024, // also the most the graphics thread interprets in one go
};

// (The game's GX writes check ps3GxWriteLimit and write the ring themselves: Dolphin/GX/GXFifo.h.)
extern "C" {
gpu::u8* ps3GxRing;
u32 ps3GxWritten;    // game thread only
u32 ps3GxWriteLimit; // (game thread) ps3GxWritten can grow to this without the slow path (refreshWriteLimit)
}
static_assert(PS3_GX_RING_MASK == RING_MASK, "Dolphin/GX/GXFifo.h has the ring's size");
static bool sLinked; // (the CPU FIFO goes to the graphics processor, not into a display list)
static volatile u32 sPublished;    // written by the game thread
static volatile u32 sConsumed;     // written by the graphics thread
static volatile int sConsumerWaiting;
static volatile int sProducerWaiting;
static unsigned int sMutex, sWorkCond, sSpaceCond;
static bool sThreaded;
static bool sForceSync;             // host tests
static u32 sProducerWaits;          // times the game waited for the graphics thread to make room

// The batch being interpreted (for a trace that starts in the middle of it).
static const gpu::u8* sBatch;
static u32 sBatchLen;
static gpu::u64 sBatchBase;

bool ps3gx::currentBatch(const gpu::u8** data, gpu::u32* size, gpu::u64* base)
{
	if (!sBatch) {
		return false;
	}
	*data = sBatch;
	*size = sBatchLen;
	*base = sBatchBase;
	return true;
}

static void interpret(const gpu::u8* p, u32 n)
{
	unsigned long long t0 = ps3b_timebase();
	gpu::PhaseScope phase(gpu::GFX_COMMANDS);
	sBatch                = p;
	sBatchLen             = n;
	sBatchBase            = sCp.received();
	sCp.write(p, n);
	if (ps3gx::traceRecording()) {
		gpu::PhaseScope checks(gpu::GFX_CHECKS);
		ps3gx::traceBatchDone();
	}
	sBatch = NULL;
	sCpTime += ps3b_timebase() - t0;
}

// The graphics thread also mixes the sound DSP's sub-frames (platform/ps3/sdk/audio.cpp): when it waits for
// work, and between batches of commands.
extern "C" int ps3DspMixPending(void);
extern "C" void ps3DspMixRun(void);

// Interprets the published bytes up to `end` (graphics thread, or the game thread when not threaded).
static void drain(u32 end)
{
	while (sConsumed != end) {
		u32 at = sConsumed & RING_MASK;
		u32 n  = end - sConsumed;
		if (n > RING_SIZE - at) {
			n = RING_SIZE - at;
		}
		if (n > PUBLISH_CHUNK) {
			n = PUBLISH_CHUNK;
		}
		interpret(ps3GxRing + at, n);
		__sync_synchronize(); // done reading those bytes before the game may overwrite them
		sConsumed = sConsumed + n;
		__sync_synchronize();
		if (sProducerWaiting) {
			ps3b_mutex_lock(sMutex);
			ps3b_cond_signal(sSpaceCond);
			ps3b_mutex_unlock(sMutex);
		}
		if (sThreaded) {
			ps3DspMixRun(); // (a sub-frame asked for meanwhile)
		}
	}
}

// The graphics thread's stack, for the profiler (profiler.cpp walks it from ps3GfxHook.lastSp).
extern "C" {
unsigned long long ps3GfxStackLo, ps3GfxStackHi;
}

static void graphicsThread(void*)
{
	ps3b_thread_stack(&ps3GfxStackLo, &ps3GfxStackHi);
	for (;;) {
		ps3b_mutex_lock(sMutex);
		sConsumerWaiting = 1;
		__sync_synchronize();
		while (sPublished == sConsumed && !ps3DspMixPending()) {
			ps3b_cond_wait(sWorkCond, 5000);
		}
		sConsumerWaiting = 0;
		ps3b_mutex_unlock(sMutex);
		__sync_synchronize();
		ps3DspMixRun();
		drain(sPublished);
	}
}

extern "C" int ps3GxThreaded(void) { return sThreaded; }

extern "C" void ps3GxWake(void)
{
	if (!sThreaded) {
		return;
	}
	ps3b_mutex_lock(sMutex);
	if (sConsumerWaiting) {
		ps3b_cond_signal(sWorkCond);
	}
	ps3b_mutex_unlock(sMutex);
}

static void refreshWriteLimit();

static void publish()
{
	if (!ps3GxRing || sPublished == ps3GxWritten) {
		refreshWriteLimit();
		return;
	}
	__sync_synchronize(); // the bytes before the counter
	sPublished = ps3GxWritten;
	refreshWriteLimit();
	if (!sThreaded) {
		drain(ps3GxWritten);
		refreshWriteLimit();
		return;
	}
	__sync_synchronize();
	if (sConsumerWaiting) {
		ps3b_mutex_lock(sMutex);
		ps3b_cond_signal(sWorkCond);
		ps3b_mutex_unlock(sMutex);
	}
}

static void makeRoom(u32 n)
{
	publish();
	if (!sThreaded) {
		return; // (publish interpreted everything)
	}
	sProducerWaits++;
	ps3b_mutex_lock(sMutex);
	sProducerWaiting = 1;
	__sync_synchronize();
	while (ps3GxWritten - sConsumed + n > RING_SIZE) {
		ps3b_cond_wait(sSpaceCond, 5000);
	}
	sProducerWaiting = 0;
	ps3b_mutex_unlock(sMutex);
}

// How far the game thread can write before put's checks are needed: not past what the graphics thread has
// yet to read, not past the end of the ring, and not as far as the next publishing point.
static void refreshWriteLimit()
{
	const u32 w = ps3GxWritten;
	if (!sLinked || !ps3GxRing) {
		ps3GxWriteLimit = w; // (every write goes through ps3GxWrite*)
		return;
	}
	u32 room      = sConsumed + RING_SIZE - w;
	const u32 end = RING_SIZE - (w & RING_MASK);
	const u32 out = w - sPublished; // (written, not yet published)
	const u32 pub = out < PUBLISH_CHUNK ? PUBLISH_CHUNK - 1 - out : 0;
	if (end < room) {
		room = end;
	}
	if (pub < room) {
		room = pub;
	}
	ps3GxWriteLimit = w + (room <= RING_SIZE ? room : 0);
}

static void put(const gpu::u8* b, u32 n)
{
	if (ps3GxWritten - sConsumed + n > RING_SIZE) {
		makeRoom(n);
	}
	u32 at = ps3GxWritten & RING_MASK;
	if (at + n <= RING_SIZE) {
		for (u32 i = 0; i < n; i++) {
			ps3GxRing[at + i] = b[i];
		}
	} else {
		for (u32 i = 0; i < n; i++) {
			ps3GxRing[(ps3GxWritten + i) & RING_MASK] = b[i];
		}
	}
	ps3GxWritten += n;
	if (ps3GxWritten - sPublished >= PUBLISH_CHUNK) {
		publish();
	}
	refreshWriteLimit();
}

// The writes the GX library makes (every command word it sends): straight into the ring while there is room
// before the limit, otherwise through put. (Test build 32 built each word in a small array on the stack and
// copied it from there, a byte at a time, with the room and publishing checks on every write.)
static inline bool putFast(u32 value, u32 n)
{
	const u32 w = ps3GxWritten;
	if (ps3GxWriteLimit - w < n || !ps3GxRing) {
		return false;
	}
	gpu::u8* p = ps3GxRing + (w & RING_MASK);
	switch (n) {
	case 4: p[0] = (gpu::u8)(value >> 24), p[1] = (gpu::u8)(value >> 16), p[2] = (gpu::u8)(value >> 8), p[3] = (gpu::u8)value; break;
	case 2: p[0] = (gpu::u8)(value >> 8), p[1] = (gpu::u8)value; break;
	default: p[0] = (gpu::u8)value; break;
	}
	ps3GxWritten = w + n;
	return true;
}

void ps3gx::finish()
{
	publish();
	while (sConsumed != ps3GxWritten) {
		ps3b_sleep_us(100);
	}
}

void ps3gx::forceSynchronous() { sForceSync = true; }

// ================================================================== the CPU FIFO

static __GXFifoObj* sCpuFifo;

static void start()
{
	sStarted = true;
	ps3GfxPhaseSince = gpu::gfxClock();
	sArenaLo = (uptr)OSGetArenaLo();
	sArenaHi = (uptr)OSGetArenaHi();
	sCp.setMemory(&sMemory);
	sCp.setListener(&sEvents);
	if (!sRenderer) {
		sRenderer      = ps3rsx::start(sCp, sMemory);
		sRendererIsRsx = sRenderer != NULL;
	}
	sCp.setRegisterListener(sRenderer);
	ps3GxRing = (gpu::u8*)ps3b_memalign(128, RING_SIZE);
	if (!ps3GxRing) {
		ps3Log("[gpu] FAILED: no memory for the graphics command buffer\n");
		return;
	}
	bool sync = sForceSync;
	if (!sync && ps3b_data_root()) {
		char path[256];
		snprintf(path, sizeof(path), "%s/gx_sync.txt", ps3b_data_root());
		sync = ps3b_file_exists(path) != 0;
	}
	if (!sync && ps3b_mutex_create(&sMutex) == 0 && ps3b_cond_create(&sWorkCond, sMutex) == 0
	    && ps3b_cond_create(&sSpaceCond, sMutex) == 0
	    && ps3b_thread_create(graphicsThread, NULL, 1500, 0x10000, "pikmin_gpu") == 0) {
		sThreaded = true;
	}
	ps3Log("[gpu] graphics commands are interpreted %s and %s; game memory 0x%08x-0x%08x\n",
	       sThreaded ? "on the graphics thread" : "on the game's thread", sRendererIsRsx ? "drawn by the RSX" : "not drawn",
	       (unsigned)sArenaLo, (unsigned)sArenaHi);
}

extern "C" void ps3GxSetCpuFifo(__GXFifoObj* fifo, int linkedToGp)
{
	if (!sStarted) {
		start();
	}
	publish(); // what was written so far goes to the graphics processor first
	sCpuFifo             = fifo;
	sLinked              = linkedToGp != 0 && ps3GxRing;
	sPiRegs[PI_FIFO_PTR] = 0; // clears the overflow (wrap) bit GXEndDisplayList checks
	refreshWriteLimit();
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

// GXFlush (which the library calls before it waits for the graphics processor, e.g. in GXDrawDone).
extern "C" void ps3GxFlush(void) { publish(); }

extern "C" void ps3GxWrite8(unsigned int value)
{
	if (sLinked && putFast(value, 1)) {
		return;
	}
	u8 b = (u8)value;
	if (sLinked) {
		put(&b, 1);
	} else {
		store(&b, 1);
	}
}

extern "C" void ps3GxWrite16(unsigned int value)
{
	if (sLinked && putFast(value, 2)) {
		return;
	}
	u8 b[2] = { (u8)(value >> 8), (u8)value };
	if (sLinked) {
		put(b, 2);
	} else {
		store(b, 2);
	}
}

extern "C" void ps3GxWriteBytes(const void* bytes, unsigned int n)
{
	if (sLinked) {
		put((const u8*)bytes, n);
	} else {
		store((const u8*)bytes, n);
	}
}

extern "C" void ps3GxWrite32(unsigned int value)
{
	if (sLinked && putFast(value, 4)) {
		return;
	}
	u8 b[4] = { (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value };
	if (sLinked) {
		put(b, 4);
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
	char pending[128];
	u32 queued = ps3GxWritten - sConsumed;
	if (sCp.idle()) {
		snprintf(pending, sizeof(pending), "processor idle, %u KB queued", (unsigned)(queued >> 10));
	} else {
		snprintf(pending, sizeof(pending), "processor holds %u bytes of an unfinished command 0x%02x, %u KB queued",
		         (unsigned)sCp.pendingBytes(), (unsigned)sCp.pendingOpcode(), (unsigned)(queued >> 10));
	}
	// Time spent interpreting commands, per frame and as a share of the time, since the last report.
	unsigned long long now   = ps3b_timebase();
	u32 frames               = sFrames - sFramesReported;
	unsigned long long ticks = sCpTime - sCpTimeReported;
	unsigned long long span  = sReportedAt ? now - sReportedAt : 0;
	sFramesReported          = sFrames;
	sCpTimeReported          = sCpTime;
	sReportedAt              = now;
	unsigned long long perFrameUs = frames ? ticks * 1000000ULL / ps3b_timebase_freq() / frames : 0;
	unsigned busy                 = span ? (unsigned)(ticks * 100 / span) : 0;
	snprintf(buf, size,
	         "graphics: %u frames (last: %u draws, %u vertices, %u display lists; %u.%u ms per frame interpreting commands, "
	         "%s %u%% busy); %u TEV setups, %u vertex formats, textures %s (largest %u KB); %llu bad commands, %llu memory "
	         "failures (%u textures; checked 1 frame in %d), %u unnoted and %u ambiguous texture addresses; %llu draws, "
	         "%llu draw-done signals; %s, game waited for room %u times; register loads that changed nothing (not passed on): "
	         "%llu of %llu BP, %llu of %llu XF, %llu of %llu CP; %s",
	         (unsigned)sFrames, sLastDraws, sLastVerts, sLastDls, (unsigned)(perFrameUs / 1000), (unsigned)(perFrameUs % 1000 / 100),
	         sThreaded ? "graphics thread" : "game thread", busy, sTevSetups.count, sVertexFormats.count,
	         formats[0] ? formats : "none", sMaxTextureSize >> 10, (unsigned long long)st.badCommands,
	         (unsigned long long)st.memoryFailures, sTextureFailures, (int)CHECK_EVERY, sUnnoted, sAmbiguous,
	         (unsigned long long)st.draws, (unsigned long long)st.drawDones, pending, sProducerWaits,
	         (unsigned long long)st.bpUnchanged, (unsigned long long)st.bpLoads, (unsigned long long)st.xfUnchanged,
	         (unsigned long long)st.xfLoads, (unsigned long long)st.cpUnchanged, (unsigned long long)st.cpLoads, trace);
}

extern "C" void ps3RsxStatus(char* buf, unsigned int size) { ps3rsx::status(buf, size); }

// Time per frame in each phase of the graphics thread (gpu_cp.h), since the last report.
extern "C" void ps3GfxPhaseStatus(char* buf, unsigned int size)
{
	static unsigned long long last[gpu::GFX_PHASE_COUNT];
	static u32 lastFrames;
	const u32 frames = sFrames - lastFrames;
	lastFrames       = sFrames;
	const unsigned long long freq = ps3b_timebase_freq();
	int n = snprintf(buf, size, "graphics thread, ms per frame:");
	for (int i = 0; i < gpu::GFX_PHASE_COUNT && n < (int)size; i++) {
		const unsigned long long t = ps3GfxPhaseTicks[i];
		const unsigned long long d = t - last[i];
		last[i]                    = t;
		const unsigned long long us = frames ? d * 1000000ULL / freq / frames : 0;
		n += snprintf(buf + n, size - n, " %s %u.%u%s", gpu::kGfxPhaseNames[i], (unsigned)(us / 1000), (unsigned)(us % 1000 / 100),
		              i + 1 < gpu::GFX_PHASE_COUNT ? "," : "");
	}
}
