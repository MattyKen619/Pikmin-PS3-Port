// PS3 port: the audio DSP's mixing on an SPU (test build 44).
//
// Up to test build 43 the PPU mixed the sound (dsp_hle.cpp), on the graphics thread between its batches of
// graphics commands: 250-300 us per 2.5 ms sub-frame, 10-12% of the time, on the thread that limits the frame
// rate in the busiest places. The mixing itself is the same code (dsp_mix.h); here it reaches main memory by
// DMA.
//
// The PPU (platform/ps3/sdk/audio.cpp, dsp_hle.cpp) writes commands into a shared block (Ps3DspShared,
// ps3/dsp_shared.h) and writes signal notification register 1 to wake this program. It then mixes every
// sub-frame asked for, writes its progress and log lines back into the shared block, and sends an event per
// sub-frame; a PPU thread then raises the DSP's interrupt for the game (platform/ps3/backend/ps3_spu.cpp).
//
// DMA: a transfer must be 1, 2, 4 or 8 bytes, naturally aligned, or a multiple of 16 bytes, at local store and
// main memory addresses with the same lowest 4 bits. Reads fetch the whole 16-byte lines around the data (the
// extra bytes are not used); writes go out in pieces that cover exactly the bytes asked for.

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <sys/spu_event.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t u64;

#include "dsp_mix.h"

namespace {

enum {
	TAG   = 1,
	STAGE = 4096, // largest piece read or written at once (a sub-frame's PCM16 needs up to 2.6 KB)
};

u8 sStage[STAGE + 32] __attribute__((aligned(128)));
u8 sLine[128] __attribute__((aligned(128)));
Ps3DspLogEntry sLogBuf __attribute__((aligned(16)));
Ps3DspStats sStatsBuf __attribute__((aligned(16)));
u64 sShared;           // the shared block's address
Ps3DspShared sCtrl;    // (only its first 128 bytes: the PPU's commands)
u32 sLogWrite, sLogDropped;

inline void dmaWait()
{
	mfc_write_tag_mask(1u << TAG);
	mfc_read_tag_status_all();
}

// Main memory [ea, ea + size) into the staging buffer (size <= STAGE): returns where the first byte is.
const u8* fetch(u64 ea, u32 size)
{
	const u64 start = ea & ~15ull;
	const u32 total = (u32)(((ea + size + 15) & ~15ull) - start);
	mfc_get(sStage, start, total, TAG, 0, 0);
	dmaWait();
	return sStage + (ea & 15);
}

// Writes local store [ls, ls + size) to main memory at ea, exactly those bytes. (ls and ea have the same lowest
// 4 bits; size <= 16 KB.)
void putExact(const u8* ls, u64 ea, u32 size)
{
	while (size) {
		u32 n;
		if (!(ea & 15) && size >= 16) {
			n = size & ~15u;
		} else if (!(ea & 7) && size >= 8) {
			n = 8;
		} else if (!(ea & 3) && size >= 4) {
			n = 4;
		} else if (!(ea & 1) && size >= 2) {
			n = 2;
		} else {
			n = 1;
		}
		mfc_put((volatile void*)ls, ea, n, TAG, 0, 0);
		ls += n;
		ea += n;
		size -= n;
	}
	dmaWait();
}

struct SpuMem {
	u32 aramEa, aramSize;

	void read(void* dst, u32 address, u32 size)
	{
		u8* d = (u8*)dst;
		while (size) {
			const u32 n = size < STAGE - 16 ? size : STAGE - 16;
			memcpy(d, fetch(address, n), n);
			d += n;
			address += n;
			size -= n;
		}
	}

	void write(u32 address, const void* src, u32 size)
	{
		const u8* s = (const u8*)src;
		while (size) {
			const u32 n = size < STAGE - 16 ? size : STAGE - 16;
			u8* ls      = sStage + (address & 15);
			memcpy(ls, s, n);
			putExact(ls, address, n);
			s += n;
			address += n;
			size -= n;
		}
	}

	const u8* aram(u32 address, u32 size)
	{
		if (!aramEa || (u64)address + size > aramSize || size > STAGE - 16) {
			return NULL;
		}
		return fetch((u64)aramEa + address, size);
	}

	u32 ticks() { return ~spu_read_decrementer(); }

	void log(const Ps3DspLogEntry& e)
	{
		if (sLogWrite - sCtrl.logRead >= PS3_DSP_LOG_ENTRIES) {
			sLogDropped++; // (the PPU has not printed the earlier ones yet)
			return;
		}
		sLogBuf = e;
		mfc_put(&sLogBuf, sShared + PS3_DSP_LOG_OFFSET + (sLogWrite % PS3_DSP_LOG_ENTRIES) * sizeof(Ps3DspLogEntry), sizeof(sLogBuf),
		        TAG, 0, 0);
		dmaWait();
		sLogWrite++;
	}
};

dspmix::Mixer<SpuMem> sMixer;

// The PPU's commands: one line, read atomically (the PPU writes a command's sequence number after its values).
void readCommands()
{
	mfc_getllar(sLine, sShared, 0, 0);
	(void)mfc_read_atomic_status();
	memcpy(&sCtrl, sLine, sizeof(sLine));
}

void writeProgress()
{
	sMixer.st.logWrite   = sLogWrite;
	sMixer.st.logDropped = sLogDropped;
	sStatsBuf            = sMixer.st;
	mfc_put(&sStatsBuf, sShared + PS3_DSP_STATS_OFFSET, sizeof(sStatsBuf), TAG, 0, 0);
	dmaWait();
}

} // namespace

static_assert(sizeof(Ps3DspLogEntry) == 32 && sizeof(Ps3DspStats) == 64, "shared block layout");
static_assert(__builtin_offsetof(Ps3DspShared, stopSeq) == PS3_DSP_STOPSEQ_OFFSET, "shared block layout");
static_assert(__builtin_offsetof(Ps3DspShared, spu) == PS3_DSP_STATS_OFFSET, "shared block layout");
static_assert(__builtin_offsetof(Ps3DspShared, log) == PS3_DSP_LOG_OFFSET, "shared block layout");

namespace {

u32 sSetupSeq, sFrameSeq;

void start(u64 shared)
{
	sShared = shared;
	spu_write_decrementer(0xFFFFFFFFu);
	sMixer.reset();
	sMixer.setStops((u32)(shared + PS3_DSP_STOPSEQ_OFFSET));
	readCommands();
	sMixer.st.done = sCtrl.asked;
	sSetupSeq      = 0;
	sFrameSeq      = 0;
}

// Mixes every sub-frame asked for.
void serve()
{
	for (;;) {
		readCommands();
		sMixer.mem.aramEa   = sCtrl.aram;
		sMixer.mem.aramSize = sCtrl.aramSize;
		if (sCtrl.setupSeq != sSetupSeq) {
			sSetupSeq = sCtrl.setupSeq;
			sMixer.setup(sCtrl.voiceCount, sCtrl.voices, sCtrl.tables, sCtrl.afcCoeffs, sCtrl.reverb, sCtrl.fx);
		}
		if (sCtrl.frameSeq != sFrameSeq) {
			sFrameSeq = sCtrl.frameSeq;
			sMixer.startFrame(sCtrl.subframes, sCtrl.outputVolume, sCtrl.outL, sCtrl.outR);
		}
		if (sMixer.st.done == sCtrl.asked) {
			return;
		}
		sMixer.renderSubframe();
		sMixer.st.done++;
		writeProgress();
		spu_thread_send_event(PS3_DSP_EVENT_PORT, sMixer.st.done & 0xFFFFFF, 0);
	}
}

} // namespace

#ifndef DSP_SPU_HOST_TEST
int main(u64 shared, u64, u64, u64)
{
	start(shared);
	for (;;) {
		(void)spu_read_signal1(); // (waits for the PPU)
		serve();
	}
	return 0;
}
#else
// tools/ps3/dsp_test: this program on the host, with DMA emulated (and its rules checked).
void dspSpuHostStart(u64 shared) { start(shared); }
void dspSpuHostServe() { serve(); }
#endif
