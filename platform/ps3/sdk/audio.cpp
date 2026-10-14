// PS3 port: audio interface (AI), audio DSP mailbox, and sound output.
//
// JAudio's audio thread is paced by two GameCube interrupts:
//
//   AI DMA  - the audio interface started playing a block of samples (560 stereo frames at 32028.5 Hz,
//             17.5 ms): AudioSync() asks the thread to mix the next block.
//   DSP     - the DSP finished one of the 7 sub-frames of a mixing frame: DspSync() lets the thread
//             update voices for the next one.
//
// A timer thread raises the AI interrupt. At each block it also takes the block the game has queued
// with AIInitDMA (16-bit stereo, right channel first, as the GameCube's audio interface plays it),
// resamples it to 48 kHz and puts it in a ring that the PS3 sound thread (backend/ps3_audio.cpp) plays.
// While sound output runs, the ring's fill level paces the timer, so the game produces audio exactly as
// fast as the PS3 plays it. Without sound output, the PS3 clock paces it.
//
// The DSP side answers the microcode's mailbox protocol (boot handshake, command acknowledgements, one
// sync interrupt per released sub-frame) and mixes each sub-frame with dsp_hle.cpp.

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#include "Dolphin/ai.h"
#include "Dolphin/dsp.h"
#include "Dolphin/os.h"
#include "jaudio/syncstream.h"
#include <stdio.h>

extern "C" void ps3RaiseInterrupt(int interrupt);
extern "C" void ps3DspSetup(u32 voiceCount, u32 voices, u32 tables, u32 afcCoeffs, u32 reverb);
extern "C" void ps3DspStartFrame(u32 subframes, u32 outputVolume, u32 left, u32 right);
extern "C" void ps3DspRenderSubframe(void);
extern "C" void ps3DspStatus(char* buf, unsigned int size);
extern "C" StreamCtrl_* const ps3StreamControl; // syncstream.c

// ================================================================== AI: audio DMA timer

static volatile AIDCallback sAIDCallback;
static volatile u32 sDmaAddr;
static volatile u32 sDmaLength = 2240; // bytes; JAudio uses 560 stereo 16-bit frames
static volatile u32 sDspSampleRate = AI_SAMPLERATE_32KHZ;
static volatile u32 sAiPending;
static volatile u32 sAiBlocks;
static int sAiThreadStarted;

static void aiDmaInterrupt(void*)
{
	__sync_fetch_and_sub(&sAiPending, 1);
	sAiBlocks++;
	AIDCallback cb = sAIDCallback;
	if (cb) {
		cb();
	}
}

// ---- sound output ring: 48 kHz stereo floats, written by the timer thread, read by the sound thread

enum {
	RING_FRAMES  = 16384, // about 340 ms
	TARGET_QUEUE = 2400,  // frames kept queued while sound output runs (50 ms)
};

static float sRing[RING_FRAMES * 2];
static volatile u32 sRingWrite; // frames written (wraps)
static volatile u32 sRingRead;  // frames read
static volatile unsigned long long sLastPullTb;
static volatile u32 sUnderruns, sOverruns;

// Resampler state: position in source frames relative to the current block (-1 <= pos < 0 between
// blocks: interpolate from the previous block's last frame).
static double sSrcPos = -1.0;
static float sPrevL, sPrevR;

static void pushDacBlock(const s16* src, u32 frames, double srcRate)
{
	const double step = srcRate / 48000.0;
	double pos        = sSrcPos;
	u32 w             = sRingWrite;
	u32 room          = RING_FRAMES - (w - sRingRead);
	while (pos < (double)(frames - 1)) {
		int i       = (int)(pos + 1.0) - 1; // floor, pos >= -1
		float frac  = (float)(pos - i);
		float aL    = i < 0 ? sPrevL : src[2 * i + 1];
		float aR    = i < 0 ? sPrevR : src[2 * i];
		float bL    = src[2 * (i + 1) + 1];
		float bR    = src[2 * (i + 1)];
		if (room) {
			float* out = &sRing[(w % RING_FRAMES) * 2];
			out[0]     = (aL + (bL - aL) * frac) * (1.0f / 32768.0f);
			out[1]     = (aR + (bR - aR) * frac) * (1.0f / 32768.0f);
			w++;
			room--;
		} else {
			sOverruns++;
		}
		pos += step;
	}
	sSrcPos = pos - frames;
	sPrevL  = src[2 * (frames - 1) + 1];
	sPrevR  = src[2 * (frames - 1)];
	__sync_synchronize(); // samples before the index
	sRingWrite = w;
}

// Runs on the sound thread.
static unsigned int pullOutput(float* dst, unsigned int frames)
{
	sLastPullTb = ps3b_timebase();
	u32 r       = sRingRead;
	u32 avail   = sRingWrite - r;
	__sync_synchronize(); // index before the samples
	u32 n = avail < frames ? avail : frames;
	for (u32 i = 0; i < n; i++) {
		const float* in = &sRing[((r + i) % RING_FRAMES) * 2];
		dst[2 * i]      = in[0];
		dst[2 * i + 1]  = in[1];
	}
	__sync_synchronize();
	sRingRead = r + n;
	if (n < frames && sAiBlocks > 100) {
		sUnderruns++;
	}
	return n;
}

static bool outputRunning(unsigned long long now, unsigned long long freq)
{
	unsigned long long last = sLastPullTb;
	return last != 0 && now - last < freq / 5; // the sound thread pulled within the last 200 ms
}

static void aiTimerThread(void*)
{
	const unsigned long long freq = ps3b_timebase_freq();
	unsigned long long next       = ps3b_timebase();
	for (;;) {
		// One DMA block lasts length/4 stereo frames at the DSP sample rate (the GameCube's "32 kHz"
		// is really 32028.5 Hz).
		unsigned long long frames = sDmaLength / 4;
		if (frames == 0) {
			frames = 560;
		}
		const bool rate48 = sDspSampleRate == AI_SAMPLERATE_48KHZ;
		unsigned long long period = rate48 ? freq * frames / 48000ULL : freq * frames * 2ULL / 64057ULL;
		unsigned long long now    = ps3b_timebase();
		const bool output         = outputRunning(now, freq);
		if (output) {
			// The PS3's audio clock sets the pace: wait until the queue runs low.
			while (sRingWrite - sRingRead > TARGET_QUEUE && outputRunning(ps3b_timebase(), freq)) {
				ps3b_sleep_us(1000);
			}
			next = ps3b_timebase();
		} else {
			next += period;
			if (now < next) {
				ps3b_sleep_us((unsigned int)((next - now) * 1000000ULL / freq) + 1);
			} else if (now > next + period * 8) {
				next = now; // fell far behind: don't try to catch up
			}
		}

		// The block that starts playing now is the one the game queued last.
		u32 addr = sDmaAddr;
		u32 len  = sDmaLength;
		if (output && addr && len >= 4) {
			pushDacBlock((const s16*)(uptr)addr, len / 4, rate48 ? 48000.0 : 32028.5);
		}

		if (sAiPending < 2) {
			__sync_fetch_and_add(&sAiPending, 1);
			ps3PostInterrupt(aiDmaInterrupt, NULL);
		}
	}
}

void AIInit(u8* stack)
{
	(void)stack;
	PS3_ACTIVITY("AIInit");
}

AIDCallback AIRegisterDMACallback(AIDCallback callback)
{
	BOOL enabled    = OSDisableInterrupts();
	AIDCallback old = sAIDCallback;
	sAIDCallback    = callback;
	OSRestoreInterrupts(enabled);
	return old;
}

// The next block to play.
void AIInitDMA(u32 address, u32 length)
{
	sDmaAddr   = address;
	sDmaLength = length;
}

void AIStartDMA(void)
{
	PS3_ACTIVITY("AIStartDMA");
	if (!sAiThreadStarted) {
		sAiThreadStarted = 1;
		ps3b_audio_start(pullOutput);
		ps3b_thread_create(aiTimerThread, NULL, 900, 0x8000, "pikmin_ai");
		ps3Log("[ps3] audio: interface timer started\n");
	}
}

void AISetDSPSampleRate(u32 rate) { sDspSampleRate = rate; }
u32 AIGetDSPSampleRate(void) { return sDspSampleRate; }

// ================================================================== DSP: mailbox
//
// Mailbox protocol of JAudio's microcode (src/jaudio/dspboot.c, dspproc.c):
//   boot:     DSP ROM sends 0x8071FEED; the CPU answers with 10 mails describing the microcode; the
//             microcode then sends one "ready" mail.
//   commands: the first mail's top byte is the command; it has a fixed number of words. When a
//             command is complete the DSP acknowledges with one mail (DSPWaitFinish reads it; any value
//             except 0x88881357 counts as "done").
//   frames:   command 0x82 starts a mixing frame of N sub-frames. Before each sub-frame the CPU sends
//             mail 0 (DSPReleaseHalt); when the sub-frame is done the DSP raises its interrupt.
//             Exactly N interrupts per frame: one more would make the audio thread exit.

enum { DSP_ROM, DSP_BOOTING, DSP_RUNNING };

static int sDspState = DSP_ROM;
static int sBootMails;
static u32 sOutbox[16];
static u32 sOutHead, sOutTail;
static u32 sCmd[8];
static int sCmdHave, sCmdNeed;
static int sSubframesLeft;
static u32 sDspFrames, sDspSyncs;
static int sDspInitDone;

static void dspInit()
{
	if (!sDspInitDone) {
		sDspInitDone = 1;
		sOutbox[sOutTail++ % 16] = 0x8071FEED; // DSP ROM: ready to receive a microcode
	}
}

static void dspReply(u32 mail)
{
	if (sOutTail - sOutHead >= 16) {
		ps3Log("[ps3] FAILED: DSP mailbox overflow\n");
		return;
	}
	sOutbox[sOutTail++ % 16] = mail;
}

static int commandLength(u32 id)
{
	switch (id) {
	case 0x80: return 1; // wait for frame
	case 0x81: return 5; // set up a table
	case 0x82: return 3; // mix a frame: sub-frame count | mixer level, buffer start, buffer end
	case 0x8B: return 2; // IPL (memory card) decode section
	case 0x8C: return 2; // GBA decode section
	default:
	{
		static int sLogged;
		ps3LogOnce(&sLogged, "[ps3] audio: unknown DSP command 0x%02x, treated as one word\n", (unsigned)id);
		return 1;
	}
	}
}

static void runCommand()
{
	u32 id = sCmd[0] >> 24;
	if (id == 0x81) {
		ps3DspSetup(sCmd[0] & 0xFFFF, sCmd[1], sCmd[2], sCmd[3], sCmd[4]);
	}
	if (id == 0x82) {
		sSubframesLeft = (sCmd[0] >> 16) & 0xFF;
		sDspFrames++;
		ps3DspStartFrame(sSubframesLeft, sCmd[0] & 0xFFFF, sCmd[1], sCmd[2]);
	}
	dspReply(0xF3550000 | (id << 8)); // acknowledgement
}

u32 DSPCheckMailToDSP(void)
{
	dspInit();
	return 0; // the null DSP takes every mail at once
}

u32 DSPCheckMailFromDSP(void)
{
	dspInit();
	static u32 sEmptyPolls; // consecutive polls of an empty mailbox
	if (sOutHead != sOutTail) {
		sEmptyPolls = 0;
		return 1;
	}
	// Polling an empty mailbox: let interrupts in, and report if JAudio waits for a mail that the
	// DSP will never send. (DSPReleaseHalt polls once per sub-frame and finds nothing: normal.)
	if (++sEmptyPolls == 10000000) {
		ps3Log("[ps3] FAILED: audio code is waiting for a DSP mail that will not come\n");
	}
	ps3DeliverInterrupts();
	return 0;
}

u32 DSPReadMailFromDSP(void)
{
	dspInit();
	if (sOutHead == sOutTail) {
		return 0;
	}
	return sOutbox[sOutHead++ % 16];
}

void DSPSendMailToDSP(u32 mail)
{
	dspInit();
	switch (sDspState) {
	case DSP_ROM:
		// The CPU describes the microcode in 10 mails (address, size, entry point...).
		if (++sBootMails == 10) {
			sDspState = DSP_RUNNING;
			dspReply(0xDCD10000); // microcode running
			PS3_ACTIVITY("DSP boot");
		}
		return;
	default:
		break;
	}
	if (sCmdNeed == 0) {
		if (mail == 0) {
			// Release the halt: the DSP mixes the next sub-frame and then interrupts the CPU.
			if (sSubframesLeft > 0) {
				sSubframesLeft--;
				sDspSyncs++;
				ps3DspRenderSubframe();
				ps3RaiseInterrupt(__OS_INTERRUPT_DSP_DSP);
			}
			return;
		}
		sCmdHave = 0;
		sCmdNeed = commandLength(mail >> 24);
	}
	sCmd[sCmdHave++] = mail;
	if (sCmdHave == sCmdNeed) {
		sCmdNeed = 0;
		runCommand();
	}
}

void DSPAssertInt(void) { }

extern "C" void ps3AudioStatus(char* buf, unsigned int size)
{
	char voices[96];
	ps3DspStatus(voices, sizeof(voices));

	// The music/movie stream (jaudio/syncstream.c): what a cutscene waits for.
	static const char* const kStates[] = { "starting", "playing", "ready", "stopping", "idle", "buffering", "paused" };
	const StreamCtrl_* sc = ps3StreamControl;
	u32 state             = sc->playbackState;
	u32 id                = sc->streamId;
	u32 loaded            = sc->samplesLoaded;
	u32 total             = sc->totalSamples;
	char stream[64];
	if (state == 4) {
		snprintf(stream, sizeof(stream), "stream idle");
	} else {
		snprintf(stream, sizeof(stream), "stream %d %s (%u of %u samples)", (int)id, state < 7 ? kStates[state] : "?",
		         (unsigned)loaded, (unsigned)total);
	}
	char output[64];
	if (outputRunning(ps3b_timebase(), ps3b_timebase_freq())) {
		snprintf(output, sizeof(output), "sound out: %u queued, %u gaps", (unsigned)(sRingWrite - sRingRead), (unsigned)sUnderruns);
	} else {
		snprintf(output, sizeof(output), "sound out: off");
	}
	snprintf(buf, size, "audio: %u AI blocks, %u DSP frames, %u DSP syncs, %s; %s; %s", (unsigned)sAiBlocks, (unsigned)sDspFrames,
	         (unsigned)sDspSyncs, voices, stream, output);
}
