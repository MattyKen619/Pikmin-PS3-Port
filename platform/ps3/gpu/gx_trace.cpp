// PS3 port: frame traces (see gx_internal.h). A few frames chosen by game section (the Nintendo logo,
// the title screen, the demo movie, the opening cutscene, gameplay) are recorded to the USB drive as
// PIKMIN/gx_trace_NN_<section>.bin, for replaying and drawing off the console.
//
// File layout (all words big-endian):
//   "GXTRACE1", u32 version (2), u32 frame, char label[32]
//   u32 cp[256], u32 bp[256], u32 xf[0x1100]          register state at the start of the frame
//   events:
//     'FIFO' u32 length, bytes (padded to 4)            command bytes, in the order written
//     'MEM ' u32 address, u32 size, u32 kind, data     a block of memory the processor read
//     'MREF' u32 address, u32 size, u32 kind, u32 off  the same block again, unchanged (data at file offset off)
//     'END ' u32 fifoBytes                             the frame is the first fifoBytes FIFO bytes (the
//                                                      last FIFO event can go on past the frame's copy)
// Version 1 (test builds 17-18) started at a batch boundary and had no fifoBytes: a replay stops at the
// first copy to the display.

#include "../sdk/sdk_internal.h"

#include "../backend/ps3_backend.h"
#include "gx_internal.h"
#include <stdio.h>
#include <string.h>

namespace ps3gx {

using gpu::u32;
using gpu::u8;

enum {
	BUFFER_SIZE  = 32 << 20,
	MAX_TRACES   = 8,
	MAX_PENDING  = 8,
	DEDUP_SLOTS  = 1 << 14,
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
static gpu::u64 sRecordStart; // FIFO stream offset where the recorded frame starts
static volatile int sWriterBusy;
static bool sWriterStarted;
static u32 sWriteLen;
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

// Dedup: last recorded copy of each (address, size, kind)
static u32 sDedupKey[DEDUP_SLOTS][3];
static u32 sDedupOffset[DEDUP_SLOTS]; // 0 = empty

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

static void stopRecording(gpu::u64 fifoBytes)
{
	closeFifo();
	put32(0x454E4420); // 'END '
	put32((u32)fifoBytes);
	sRecording = false;
}

bool traceRecording() { return sRecording; }

void traceFifo(const u8* bytes, u32 n)
{
	if (!sRecording) {
		return;
	}
	if (!sFifoLenAt) {
		put32(0x4649464F); // 'FIFO'
		sFifoLenAt = sLen;
		put32(0);
	}
	putBytes(bytes, n);
	if (sTruncated) {
		stopRecording(0xFFFFFFFF);
	}
}

void traceMemory(u32 address, u32 size, gpu::MemKind kind, const u8* data)
{
	if (!sRecording) {
		return;
	}
	closeFifo();
	u32 h = ((address >> 2) * 2654435761u ^ size * 40503u ^ (u32)kind) & (DEDUP_SLOTS - 1);
	for (int probe = 0; probe < 8; probe++) {
		u32 i = (h + probe) & (DEDUP_SLOTS - 1);
		if (!sDedupOffset[i]) {
			break;
		}
		if (sDedupKey[i][0] == address && sDedupKey[i][1] == size && sDedupKey[i][2] == (u32)kind
		    && memcmp(sBuf + sDedupOffset[i], data, size) == 0) {
			put32(0x4D524546); // 'MREF'
			put32(address);
			put32(size);
			put32((u32)kind);
			put32(sDedupOffset[i]);
			return;
		}
	}
	put32(0x4D454D20); // 'MEM '
	put32(address);
	put32(size);
	put32((u32)kind);
	u32 at = sLen;
	putBytes(data, size);
	pad4();
	if (sTruncated) {
		stopRecording(0xFFFFFFFF);
		return;
	}
	for (int probe = 0; probe < 8; probe++) {
		u32 i = (h + probe) & (DEDUP_SLOTS - 1);
		if (!sDedupOffset[i] || (sDedupKey[i][0] == address && sDedupKey[i][1] == size && sDedupKey[i][2] == (u32)kind)) {
			sDedupKey[i][0] = address;
			sDedupKey[i][1] = size;
			sDedupKey[i][2] = (u32)kind;
			sDedupOffset[i] = at;
			break;
		}
	}
}

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
				ps3Log("[gpu] frame %u recorded to PIKMIN/%s (%u KB%s)\n", (unsigned)sTraceFrame, sWriteName, (unsigned)(sWriteLen >> 10),
				       sTruncated ? ", incomplete: too large" : "");
			} else {
				ps3Log("[gpu] writing %s failed\n", path);
			}
		}
		__sync_synchronize();
		sWriterBusy = 0;
	}
}

static void handOff()
{
	sWriteLen = sLen;
	char safe[32];
	int n = 0;
	for (const char* p = sTraceLabel; *p && n < 24; p++) {
		char c = *p;
		safe[n++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) ? c : '_';
	}
	safe[n] = 0;
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
		sBufSize = size;
	}
	sLen       = 0;
	sTruncated = false;
	sFifoLenAt = 0;
	memset(sDedupOffset, 0, sizeof(sDedupOffset));
	snprintf(sTraceLabel, sizeof(sTraceLabel), "%s", label);
	sTraceFrame = frame;

	putBytes((const u8*)"GXTRACE1", 8);
	put32(2);
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
	sRecording   = true;
	sRecordStart = cp.commandEnd();
	sTraces++;

	// The frame starts just after the copy that ended the last one, in the middle of the batch of FIFO
	// bytes being interpreted: record the rest of that batch now, before the processor goes on (the
	// memory blocks it reads come after their FIFO bytes in the file).
	const u8* batch;
	u32 batchLen;
	gpu::u64 base;
	if (ps3gx::currentBatch(&batch, &batchLen, &base) && sRecordStart >= base && sRecordStart <= base + batchLen) {
		u32 skip = (u32)(sRecordStart - base);
		if (skip < batchLen) {
			traceFifo(batch + skip, batchLen - skip);
		}
	}
}

void traceFrameEnd(u32 frame, const gpu::CommandProcessor& cp)
{
	sCurrentFrame = frame;
	if (sRecording) {
		stopRecording(cp.commandEnd() - sRecordStart);
		handOff();
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

// Called by the game when it enters a section (plugPikiColin/gameflow.cpp, game.cpp): schedules frame
// recordings at moments that show a variety of the game's graphics.
extern "C" void ps3GxNoteSection(const char* name)
{
	static const struct {
		const char* section;
		const char* label;
		unsigned delay;
	} kPlan[] = {
		{ "Nintendo logo", "nintendo_logo", 30 }, { "title screen", "title", 240 }, { "movie (h4m)", "movie", 300 },
		{ "file select", "file_select", 90 },     { "intro", "cutscene", 600 },    { "gameplay", "gameplay", 300 },
		{ "gameplay", "gameplay_later", 1500 },
	};
	for (unsigned i = 0; i < sizeof(kPlan) / sizeof(kPlan[0]); i++) {
		if (strcmp(name, kPlan[i].section) == 0) {
			ps3gx::traceSchedule(kPlan[i].label, kPlan[i].delay);
		}
	}
}
