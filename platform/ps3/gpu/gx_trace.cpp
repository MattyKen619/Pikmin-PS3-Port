// PS3 port: frame traces (see gx_internal.h). A few frames chosen by game section (the Nintendo logo,
// the title screen, the demo movie, the opening cutscene, gameplay) are recorded to the USB drive as
// PIKMIN/gx_trace_NN_<section>.bin, for replaying and drawing off the console.
//
// File layout (all words big-endian):
//   "GXTRACE1", u32 version (3), u32 frame, char label[32]
//   u32 cp[256], u32 bp[256], u32 xf[0x1100]          register state at the start of the frame
//   events, in the order the command processor met them:
//     'FIFO' u32 length, bytes (padded to 4)            command bytes
//     'PAGE' u32 kind, u32 address, u32 length, bytes   memory the processor is about to read: (part of) a
//                                                      4 KB page, at its address as commands give it (the
//                                                      address bits of that kind of read, see gpu::MemKind)
//     'END ' u32 fifoBytes, u32 flags                  the frame is the first fifoBytes FIFO bytes;
//                                                      flags bit 0: incomplete (the buffer was full)
// A page is recorded before the command that reads it, and again whenever the bytes a command reads have
// changed since. Replaying the events in order, with the pages kept in a memory image, reproduces the
// frame exactly.
//
// Older versions (test builds 17-19): MEM/MREF events with whole blocks, recorded after the FIFO bytes
// that read them (version 1 also started at a batch boundary and had no fifoBytes).

#include "../sdk/sdk_internal.h"

#include "../backend/ps3_backend.h"
#include "gx_internal.h"
#include "../rsx/rsx_port.h"
#include <stdio.h>
#include <string.h>

namespace ps3gx {

using gpu::u32;
using gpu::u64;
using gpu::u8;

enum {
	BUFFER_SIZE = 8 << 20, // (the largest frame so far: 5.2 MB; test 28 ran out of memory with 32 MB here)
	MAX_TRACES  = 12, // (the plan's 8, and up to 4 asked for with the Select button)
	MAX_PENDING = 8,
	PAGE_SIZE   = 4096,
	PAGE_SLOTS  = 1 << 15,
	END_RESERVE = 16, // bytes at the end of the buffer kept for the END event
};

static u8* sBuf;
static u32 sBufSize;
static u32 sLen;
static bool sRecording;
static bool sTruncated;
static u32 sFifoLenAt; // offset of the open FIFO event's length word (0 = none open)
static u32 sTraces;
static u32 sTraceBytes;
static u32 sTraceFrame;
static char sTraceLabel[32];
static u64 sRecordStart;    // FIFO stream offset where the recorded frame starts
static u64 sFifoRecordedTo; // FIFO stream offset up to which bytes are in the trace
static volatile int sWriterBusy;
static bool sWriterStarted;
static u32 sWriteLen;
static u32 sWriteFrame;
static bool sWriteTruncated;
static char sWriteName[64];

struct Pending {
	char label[32];
	u32 frame;
};
// Scheduled recordings: the game's thread adds them, the graphics thread starts them.
static Pending sPending[MAX_PENDING];
static int sPendingCount;
static char sUsed[16][32];
static int sUsedCount;
static volatile u32 sCurrentFrame;
static volatile int sScheduleLock;

// Pages in the trace: (kind, page) -> where their latest recorded bytes are.
static u32 sPageKey[PAGE_SLOTS]; // (kind group << 20 | address >> 12) + 1; 0 = empty
static u32 sPageData[PAGE_SLOTS]; // offset of the bytes in sBuf
static u16 sPageLo[PAGE_SLOTS];   // the recorded part of the page: [lo, lo + len)
static u16 sPageLen[PAGE_SLOTS];  // (len 0 means 4096)
static u32 sPageChecked[PAGE_SLOTS]; // batch in which the page was last found unchanged (or recorded)
static u32 sBatchSerial;          // FIFO batches interpreted while recording
static u32 sPageCount;
static u32 sPagesRecorded;

static void put32(u32 v)
{
	if (sLen + 4 > sBufSize) {
		sTruncated = true;
		return;
	}
	sBuf[sLen++] = (u8)(v >> 24);
	sBuf[sLen++] = (u8)(v >> 16);
	sBuf[sLen++] = (u8)(v >> 8);
	sBuf[sLen++] = (u8)v;
}

static void putBytes(const u8* p, u32 n)
{
	if (sLen + n > sBufSize) {
		sTruncated = true;
		return;
	}
	memcpy(sBuf + sLen, p, n);
	sLen += n;
}

static void pad4()
{
	while ((sLen & 3) && sLen < sBufSize) {
		sBuf[sLen++] = 0;
	}
}

static void closeFifo()
{
	if (!sFifoLenAt) {
		return;
	}
	u32 len = sLen - (sFifoLenAt + 4);
	sBuf[sFifoLenAt]     = (u8)(len >> 24);
	sBuf[sFifoLenAt + 1] = (u8)(len >> 16);
	sBuf[sFifoLenAt + 2] = (u8)(len >> 8);
	sBuf[sFifoLenAt + 3] = (u8)len;
	pad4();
	sFifoLenAt = 0;
}

bool traceRecording() { return sRecording; }

// ---------------------------------------------------------------- writing to the USB drive

static void writerThread(void*)
{
	for (;;) {
		while (!sWriterBusy) {
			ps3b_sleep_us(50 * 1000);
		}
		char path[256];
		snprintf(path, sizeof(path), "%s/%s", ps3b_data_root(), sWriteName);
		int fd = ps3b_file_open_rw(path, 1);
		if (fd < 0) {
			ps3Log("[gpu] could not create %s\n", path);
		} else {
			int wrote = ps3b_file_pwrite(fd, sBuf, 0, sWriteLen);
			ps3b_file_close(fd);
			if (wrote == (int)sWriteLen) {
				ps3Log("[gpu] frame %u recorded to PIKMIN/%s (%u KB%s)\n", (unsigned)sWriteFrame, sWriteName,
				       (unsigned)(sWriteLen >> 10), sWriteTruncated ? ", incomplete: the frame did not fit" : "");
			} else {
				ps3Log("[gpu] writing %s failed\n", path);
			}
		}
		__sync_synchronize();
		sWriterBusy = 0;
	}
}

static void safeLabel(char* safe)
{
	int n = 0;
	for (const char* p = sTraceLabel; *p && n < 24; p++) {
		char c = *p;
		safe[n++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
	}
	safe[n] = 0;
}

bool traceCurrentName(char* buf, unsigned int size)
{
	if (!sRecording) {
		return false;
	}
	char safe[32];
	safeLabel(safe);
	snprintf(buf, size, "%02u_%s", (unsigned)sTraces, safe);
	return true;
}

static void handOff()
{
	sWriteLen       = sLen;
	sWriteFrame     = sTraceFrame;
	sWriteTruncated = sTruncated;
	char safe[32];
	safeLabel(safe);
	snprintf(sWriteName, sizeof(sWriteName), "gx_trace_%02u_%s.bin", (unsigned)sTraces, safe);
	sTraceBytes += sLen;
	if (!sWriterStarted) {
		sWriterStarted = true;
		if (ps3b_thread_create(writerThread, NULL, 1450, 0x8000, "pikmin_gxtrace") != 0) {
			ps3Log("[gpu] could not start the trace writer\n");
			return;
		}
	}
	__sync_synchronize();
	sWriterBusy = 1;
}

// Ends the recording (the frame's copy to the display, or the buffer is full) and writes it out.
static void finishRecording()
{
	closeFifo();
	bool truncated = sTruncated;
	sTruncated     = false;
	sBufSize += END_RESERVE; // (kept free for this)
	put32(0x454E4420);       // 'END '
	put32((u32)(sFifoRecordedTo - sRecordStart));
	put32(truncated ? 1 : 0);
	sBufSize -= END_RESERVE;
	sTruncated = truncated;
	sRecording = false;
	handOff();
}

// ---------------------------------------------------------------- recording

// FIFO bytes of the batch being interpreted, from sFifoRecordedTo up to stream offset `to`.
static void recordFifoTo(u64 to)
{
	const u8* batch;
	u32 batchLen;
	u64 base;
	if (!ps3gx::currentBatch(&batch, &batchLen, &base)) {
		return;
	}
	if (to > base + batchLen) {
		to = base + batchLen;
	}
	if (sFifoRecordedTo < base || to <= sFifoRecordedTo) {
		return; // (bytes of an earlier batch are in the trace already)
	}
	if (!sFifoLenAt) {
		u32 start = sLen;
		put32(0x4649464F); // 'FIFO'
		put32(0);
		if (sTruncated) {
			sLen = start;
			return;
		}
		sFifoLenAt = start + 4;
	}
	putBytes(batch + (sFifoRecordedTo - base), (u32)(to - sFifoRecordedTo)); // (all or nothing)
	if (!sTruncated) {
		sFifoRecordedTo = to;
	}
}

void traceBatchDone()
{
	const u8* batch;
	u32 batchLen;
	u64 base;
	if (sRecording && ps3gx::currentBatch(&batch, &batchLen, &base)) {
		sBatchSerial++;
		recordFifoTo(base + batchLen);
		if (sTruncated) {
			finishRecording();
		}
	}
}

static u32 pageSlot(u32 key)
{
	u32 h = (key * 2654435761u) & (PAGE_SLOTS - 1);
	while (sPageKey[h] && sPageKey[h] != key) {
		h = (h + 1) & (PAGE_SLOTS - 1);
	}
	return h;
}

void traceMemory(u32 address, u32 size, gpu::MemKind kind, const u8* data, const u8* regionLo, const u8* regionHi)
{
	if (!sRecording || !size) {
		return;
	}
	// Textures and TLUTs are both 26-bit addresses into the same memory.
	const u32 group = kind == gpu::MEM_TLUT ? (u32)gpu::MEM_TEXTURE : (u32)kind;
	bool fifoDone   = false;
	u32 page        = address & ~(u32)(PAGE_SIZE - 1);
	const u32 end   = address + size;
	for (; page < end && page >= (address & ~(u32)(PAGE_SIZE - 1)); page += PAGE_SIZE) {
		// The part of the page the read uses, and the part that can be recorded (within the region).
		u32 useLo         = address > page ? address - page : 0;
		u32 useHi         = end - page < PAGE_SIZE ? end - page : PAGE_SIZE;
		const u8* realPage = data + ((long long)page - (long long)address); // (may lie before the region)
		u32 lo            = realPage >= regionLo ? 0 : (u32)(regionLo - realPage);
		u32 hi            = realPage + PAGE_SIZE <= regionHi ? PAGE_SIZE : (u32)(regionHi - realPage);
		u32 key           = ((group << 20) | (page >> 12)) + 1;
		u32 slot          = pageSlot(key);
		if (sPageKey[slot]) {
			u32 rlo = sPageLo[slot];
			u32 rhi = rlo + (sPageLen[slot] ? sPageLen[slot] : PAGE_SIZE);
			if (rlo <= useLo && useHi <= rhi) {
				// Compared at most once per batch of commands: a gameplay frame reads the same model
				// and texture pages thousands of times (test 20: comparing every read froze the game
				// for about 20 seconds per recorded frame).
				if (sPageChecked[slot] == sBatchSerial) {
					continue;
				}
				if (memcmp(sBuf + sPageData[slot] + (useLo - rlo), realPage + useLo, useHi - useLo) == 0) {
					sPageChecked[slot] = sBatchSerial;
					continue; // unchanged since recorded
				}
			}
		} else if (sPageCount >= PAGE_SLOTS * 3 / 4) {
			continue; // (table full: the read is not recorded; a replay reports it)
		}
		if (!fifoDone) {
			// The command doing this read goes after its pages.
			fifoDone = true;
			recordFifoTo(ps3gx::commandProcessor().commandStart());
			closeFifo();
		}
		if (sTruncated) {
			finishRecording();
			return;
		}
		u32 start = sLen;
		put32(0x50414745); // 'PAGE'
		put32((u32)kind);
		put32(page + lo);
		put32(hi - lo);
		u32 at = sLen;
		putBytes(realPage + lo, hi - lo);
		pad4();
		if (sTruncated) {
			sLen = start; // (not the half-written event)
			finishRecording();
			return;
		}
		if (!sPageKey[slot]) {
			sPageKey[slot] = key;
			sPageCount++;
		}
		sPageData[slot]    = at;
		sPageChecked[slot] = sBatchSerial;
		sPageLo[slot]      = (u16)lo;
		sPageLen[slot]  = (u16)((hi - lo) & 0xFFFF);
		sPagesRecorded++;
	}
}

// ---------------------------------------------------------------- frame boundaries

static void dropPending()
{
	ps3gx::spinLock(&sScheduleLock);
	sPendingCount = 0;
	ps3gx::spinUnlock(&sScheduleLock);
}

static void startRecording(const char* label, u32 frame, const gpu::CommandProcessor& cp)
{
	if (!ps3b_data_root()) {
		dropPending(); // no USB drive to write to
		return;
	}
	if (!sBuf) {
		u32 size = BUFFER_SIZE;
		while (size >= (4u << 20) && !(sBuf = (u8*)ps3b_memalign(32, size))) {
			size /= 2;
		}
		if (!sBuf) {
			ps3Log("[gpu] no memory to record a frame\n");
			dropPending();
			return;
		}
		sBufSize = size - END_RESERVE;
		ps3Log("[gpu] frame recording buffer: %u KB\n", (unsigned)(size >> 10));
		ps3b_log_memory("after the frame recording buffer");
	}
	sLen       = 0;
	sTruncated = false;
	sFifoLenAt = 0;
	memset(sPageKey, 0, sizeof(sPageKey));
	sPageCount = 0;
	sBatchSerial++;
	snprintf(sTraceLabel, sizeof(sTraceLabel), "%s", label);
	sTraceFrame = frame;

	putBytes((const u8*)"GXTRACE1", 8);
	put32(3);
	put32(frame);
	char name[32];
	memset(name, 0, sizeof(name));
	snprintf(name, sizeof(name), "%s", label);
	putBytes((const u8*)name, 32);
	const gpu::State& S = cp.state();
	for (u32 i = 0; i < 0x100; i++) {
		put32(S.cp[i]);
	}
	for (u32 i = 0; i < 0x100; i++) {
		put32(S.bp[i]);
	}
	for (u32 i = 0; i < 0x1100; i++) {
		put32(S.xf[i]);
	}
	// The frame starts just after the copy that ended the last one (in the middle of the batch being
	// interpreted); its FIFO bytes are recorded as the processor goes on.
	sRecording      = true;
	sRecordStart    = cp.commandEnd();
	sFifoRecordedTo = sRecordStart;
	sTraces++;
}

void traceFrameEnd(u32 frame, const gpu::CommandProcessor& cp)
{
	sCurrentFrame = frame;
	if (sRecording) {
		recordFifoTo(cp.commandEnd());
		finishRecording();
		return;
	}
	if (sPendingCount == 0 || sWriterBusy || sTraces >= MAX_TRACES) {
		return;
	}
	// The earliest pending recording that is due: it records the next frame, from here.
	Pending due;
	bool found = false;
	ps3gx::spinLock(&sScheduleLock);
	int best = -1;
	for (int i = 0; i < sPendingCount; i++) {
		if (sPending[i].frame <= frame && (best < 0 || sPending[i].frame < sPending[best].frame)) {
			best = i;
		}
	}
	if (best >= 0) {
		due            = sPending[best];
		sPending[best] = sPending[--sPendingCount];
		found          = true;
	}
	ps3gx::spinUnlock(&sScheduleLock);
	if (found) {
		startRecording(due.label, frame + 1, cp);
	}
}

void traceSchedule(const char* label, u32 delay)
{
	ps3gx::spinLock(&sScheduleLock);
	bool used = false;
	for (int i = 0; i < sUsedCount; i++) {
		if (strcmp(sUsed[i], label) == 0) {
			used = true;
		}
	}
	if (!used && sUsedCount < 16 && sPendingCount < MAX_PENDING) {
		snprintf(sUsed[sUsedCount++], 32, "%s", label);
		Pending& p = sPending[sPendingCount++];
		snprintf(p.label, sizeof(p.label), "%s", label);
		p.frame = sCurrentFrame + delay;
	}
	ps3gx::spinUnlock(&sScheduleLock);
}

void traceStatus(char* buf, unsigned int size)
{
	if (sTraces == 0) {
		snprintf(buf, size, "no frames recorded yet");
	} else {
		snprintf(buf, size, "%u frames recorded (%u KB)", (unsigned)sTraces, (unsigned)(sTraceBytes >> 10));
	}
}

} // namespace ps3gx

// The Select button (rsx_port.cpp): records the next frame, up to 4 times.
extern "C" void ps3GxRecordRequested(void)
{
	static unsigned sRequests;
	if (sRequests >= 4 || !ps3DiagnosticsOn()) {
		return;
	}
	char label[32];
	snprintf(label, sizeof(label), "pressed_%u", ++sRequests);
	ps3gx::traceSchedule(label, 1);
	ps3Log("[gpu] frame recording asked for with Select (%s)\n", label);
}

// Called by the game when it enters a section (plugPikiColin/gameflow.cpp, game.cpp): schedules frame
// recordings at moments that show a variety of the game's graphics.
extern "C" void ps3GxNoteSection(const char* name)
{
	static const struct {
		const char* section;
		const char* label;
		unsigned delay;
	} kPlan[] = {
		{ "boot logo", "boot_logo", 6 },          { "boot logo", "boot_logo_later", 30 },
		{ "Nintendo logo", "nintendo_logo", 30 }, { "title screen", "title", 240 }, { "movie (h4m)", "movie", 300 },
		{ "file select", "file_select", 90 },     { "intro", "cutscene", 600 },    { "gameplay", "gameplay", 300 },
		{ "gameplay", "gameplay_later", 1500 },
	};
	for (unsigned i = 0; i < sizeof(kPlan) / sizeof(kPlan[0]) && ps3DiagnosticsOn(); i++) {
		if (strcmp(name, kPlan[i].section) == 0) {
			ps3gx::traceSchedule(kPlan[i].label, kPlan[i].delay);
		}
	}
	ps3rsx::sectionChanged();
	ps3ProfileSection(name);
}
