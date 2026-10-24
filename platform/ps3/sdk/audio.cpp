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
// fast as the PS3 plays it, and the ring's 100 ms let the timer wait for a late block instead of playing
// the previous one again. Without sound output, the PS3 clock paces it.
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
#include <string.h>

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
static volatile u32 sDmaSeq;           // AIInitDMA calls: tells the timer the game has queued a new block
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
	TARGET_QUEUE = 4800,  // frames kept queued while sound output runs (100 ms)
	LOW_QUEUE    = 1200,  // waiting for a late block stops here (25 ms left), and the old block plays again
};

static float sRing[RING_FRAMES * 2];
static volatile u32 sRingWrite; // frames written (wraps)
static volatile u32 sRingRead;  // frames read
static volatile unsigned long long sLastPullTb;
static volatile u32 sUnderruns, sOverruns;

// (diagnostics, further down)
static void captureOutput(const float* src, u32 have, u32 frames);
static unsigned long long sOutputStartTb;
static volatile u32 sEarlyDropped, sEarlyUnderruns;
static volatile unsigned long long sStartTb, sFirstPullTb;

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
	if (sStartTb) {
		if (n < frames && sAiBlocks <= 400) {
			sEarlyUnderruns++;
		}
		if (!sFirstPullTb) {
			sFirstPullTb = sLastPullTb;
		}
	}
	captureOutput(dst, n, frames);
	return n;
}

// ---- diagnostics: blocks played twice because the game had no new one ready, stream stalls, JAudio's
// DSP overload check, and a recording of the first minutes of sound (PIKMIN/sound_capture.wav) for
// checking it off the console.

static volatile u32 sRepeats;
static volatile u32 sStreamStalls;
static volatile u32 sDspOverloads;
static volatile u32 sLateBlocks;  // blocks the timer waited for (the game's audio thread was late)
static volatile u32 sLateMaxUs;   // longest such wait since the last heartbeat

// jaudio/dspdriver.c: a sub-frame came about as long after the previous one as the gap between frames.
// On the GameCube that means the DSP has too much to do, and JAudio stops a voice. Here it means the
// audio thread was held up; only counted (and the first few logged). Times are in OSGetTick units
// (40.5 MHz).
extern "C" void ps3NoteDspOverload(unsigned int frameGap, unsigned int subframeTime)
{
	u32 n = ++sDspOverloads;
	if (n <= 5) {
		ps3Log("[sound] JAudio's DSP overload check fired (sub-frame %u us after the one before, frame gap %u us): "
		       "on the GameCube it would stop a voice; not on the PS3\n",
		       (unsigned)(subframeTime / 40.5), (unsigned)(frameGap / 40.5));
	}
}
static u32 sLastBlockAddr;
static u32 sLastStreamState = 4;

// Recordings of the first seconds of sound, for checking it off the console: what the game's mixer made
// (PIKMIN/sound_mix.wav, 32 kHz, the blocks the AI plays) and what went to the PS3's sound output
// (PIKMIN/sound_out.wav, 48 kHz). (Test builds 14-16 recorded the first; test 28 both: the Nintendo logo's
// sound is in both, at the volume of the music.) Build with -DPS3_SOUND_CAPTURE=1 to turn them on (8 MB).
#ifndef PS3_SOUND_CAPTURE
#define PS3_SOUND_CAPTURE 0
#endif
enum { CAPTURE_SECONDS = 25 };
struct Capture {
	u8* data;              // WAV data: little-endian 16-bit, left then right
	volatile u32 frames;   // frames recorded
	u32 max, rate;
	const char* name;
};
static Capture sMixCapture = { NULL, 0, 0, 32028, "sound_mix.wav" };
static Capture sOutCapture = { NULL, 0, 0, 48000, "sound_out.wav" };

// (src: interleaved s16, right then left as the AI plays them)
static void captureBlock(const s16* src, u32 frames)
{
	Capture& c = sMixCapture;
	if (!c.data) {
		return;
	}
	u32 n = c.frames;
	for (u32 i = 0; i < frames && n < c.max; i++, n++) {
		u16 l   = (u16)src[2 * i + 1];
		u16 r   = (u16)src[2 * i];
		u8* out = c.data + n * 4;
		out[0]  = (u8)l;
		out[1]  = (u8)(l >> 8);
		out[2]  = (u8)r;
		out[3]  = (u8)(r >> 8);
	}
	__sync_synchronize();
	c.frames = n;
}

// (src: interleaved floats, left then right, as the sound thread gets them; silence for frames missing)
static void captureOutput(const float* src, u32 have, u32 frames)
{
	Capture& c = sOutCapture;
	if (!c.data) {
		return;
	}
	u32 n = c.frames;
	for (u32 i = 0; i < frames && n < c.max; i++, n++) {
		float fl = i < have ? src[2 * i] : 0.0f, fr = i < have ? src[2 * i + 1] : 0.0f;
		s32 l = (s32)(fl * 32767.0f), r = (s32)(fr * 32767.0f);
		l = l < -32768 ? -32768 : (l > 32767 ? 32767 : l);
		r = r < -32768 ? -32768 : (r > 32767 ? 32767 : r);
		u8* out = c.data + n * 4;
		out[0]  = (u8)(u16)l;
		out[1]  = (u8)((u16)l >> 8);
		out[2]  = (u8)(u16)r;
		out[3]  = (u8)((u16)r >> 8);
	}
	__sync_synchronize();
	c.frames = n;
}

static void put32le(u8* p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

static void captureWriterThread(void*)
{
	Capture* caps[2] = { &sMixCapture, &sOutCapture };
	int fd[2];
	u32 written[2] = { 0, 0 };
	bool done[2]   = { false, false };
	for (int k = 0; k < 2; k++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", ps3b_data_root(), caps[k]->name);
		fd[k] = ps3b_file_open_rw(path, 1);
		if (fd[k] < 0) {
			ps3Log("[ps3] sound capture: could not create %s\n", path);
			done[k] = true;
		}
	}
	ps3Log("[ps3] sound capture: recording the first %d seconds to PIKMIN/sound_mix.wav and sound_out.wav\n", CAPTURE_SECONDS);
	while (!done[0] || !done[1]) {
		ps3b_sleep_us(500 * 1000);
		for (int k = 0; k < 2; k++) {
			Capture& c   = *caps[k];
			u32 frames   = c.frames;
			if (done[k] || frames == written[k]) {
				continue;
			}
			ps3b_file_pwrite(fd[k], c.data + written[k] * 4, 44 + (unsigned long long)written[k] * 4, (frames - written[k]) * 4);
			written[k] = frames;
			u8 h[44];
			memcpy(h, "RIFF", 4);
			put32le(h + 4, 36 + frames * 4);
			memcpy(h + 8, "WAVEfmt ", 8);
			put32le(h + 16, 16);
			h[20] = 1; // PCM
			h[21] = 0;
			h[22] = 2; // channels
			h[23] = 0;
			put32le(h + 24, c.rate);
			put32le(h + 28, c.rate * 4);
			h[32] = 4; // bytes per frame
			h[33] = 0;
			h[34] = 16; // bits
			h[35] = 0;
			memcpy(h + 36, "data", 4);
			put32le(h + 40, frames * 4);
			ps3b_file_pwrite(fd[k], h, 0, 44);
			if (frames >= c.max) {
				ps3b_file_close(fd[k]);
				done[k] = true;
			}
		}
	}
	ps3Log("[ps3] sound capture: done (%d seconds)\n", CAPTURE_SECONDS);
}

static void startCapture()
{
	if (!ps3b_data_root()) {
		return;
	}
	Capture* caps[2] = { &sMixCapture, &sOutCapture };
	for (int k = 0; k < 2; k++) {
		caps[k]->max  = CAPTURE_SECONDS * caps[k]->rate;
		caps[k]->data = (u8*)ps3b_memalign(32, caps[k]->max * 4);
		if (!caps[k]->data) {
			ps3Log("[ps3] sound capture: not enough memory\n");
			return;
		}
	}
	ps3b_thread_create(captureWriterThread, NULL, 1400, 0x8000, "pikmin_capture");
}

// (sEarlyDropped, sEarlyUnderruns: the first seconds of sound, where the Nintendo logo's is: blocks the game
// made while the sound output was not running, not sent on; times the output found nothing queued)

static bool outputRunning(unsigned long long now, unsigned long long freq)
{
	unsigned long long last = sLastPullTb;
	return last != 0 && now - last < freq / 5; // the sound thread pulled within the last 200 ms
}

static void aiTimerThread(void*)
{
	const unsigned long long freq = ps3b_timebase_freq();
	unsigned long long next       = ps3b_timebase();
	unsigned long long lastTick   = 0;
	u32 seqTaken                  = 0; // sDmaSeq when the last block was taken
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
			// The game answers each AI interrupt by queuing its next block (AIInitDMA). If its audio thread
			// was held up and has not answered yet, wait while the sound already queued for the PS3 lasts,
			// instead of playing the previous block a second time (a stutter, and up to test build 14 the
			// cause of JAudio stopping voices). The game then catches up: the queue is below its target, so
			// blocks are taken as fast as the game makes them, but at least 6 ms apart, so that JAudio's
			// mixer finishes each frame before the next is due (it has only one frame in reserve).
			const unsigned long long waitStart = ps3b_timebase();
			bool waited                        = false;
			while (sDmaSeq == seqTaken && sRingWrite - sRingRead > LOW_QUEUE && outputRunning(ps3b_timebase(), freq)) {
				ps3b_sleep_us(500);
				waited = true;
			}
			if (waited) {
				sLateBlocks++;
				u32 us = (u32)((ps3b_timebase() - waitStart) * 1000000ULL / freq);
				if (us > sLateMaxUs) {
					sLateMaxUs = us;
				}
			}
			const unsigned long long minGap = freq * 6 / 1000;
			unsigned long long t            = ps3b_timebase();
			if (lastTick && t - lastTick < minGap) {
				ps3b_sleep_us((unsigned int)((minGap - (t - lastTick)) * 1000000ULL / freq) + 1);
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

		// The block that starts playing now is the one the game queued last. (The same one again means
		// the game's audio thread did not get to run in time.)
		seqTaken = sDmaSeq;
		__sync_synchronize(); // the sequence number before the block it announces
		lastTick = ps3b_timebase();
		u32 addr = sDmaAddr;
		u32 len  = sDmaLength;
		if (addr && len >= 4) {
			if (addr == sLastBlockAddr && sAiBlocks > 100) {
				sRepeats++;
			}
			sLastBlockAddr = addr;
			if (output) {
				pushDacBlock((const s16*)(uptr)addr, len / 4, rate48 ? 48000.0 : 32028.5);
			} else if (sAiBlocks <= 400) {
				sEarlyDropped++;
			}
			captureBlock((const s16*)(uptr)addr, len / 4);
			static int sReported;
			const u32 blocks = sAiBlocks;
			if ((blocks >= 100 && sReported == 0) || (blocks >= 400 && sReported == 1)) {
				sReported++;
				const unsigned long long f = ps3b_timebase_freq();
				ps3Log("[ps3] audio: first %u blocks (%u ms): %u not sent on (the sound output was not running), %u times the "
				       "output found nothing queued; output first pulled %u ms after the start, which was %u ms after the sound "
				       "output started\n",
				       (unsigned)blocks, (unsigned)((ps3b_timebase() - sStartTb) * 1000 / f), (unsigned)sEarlyDropped,
				       (unsigned)sEarlyUnderruns, sFirstPullTb ? (unsigned)((sFirstPullTb - sStartTb) * 1000 / f) : 0u,
				       sOutputStartTb ? (unsigned)((sStartTb - sOutputStartTb) * 1000 / f) : 0u);
			}
		}
		u32 streamState = ps3StreamControl->playbackState;
		if (streamState == 5 && sLastStreamState != 5) {
			sStreamStalls++; // the stream ran out of data and paused
		}
		sLastStreamState = streamState;

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
	__sync_synchronize(); // the block before its sequence number
	sDmaSeq++;
}

void AIStartDMA(void)
{
	PS3_ACTIVITY("AIStartDMA");
	if (!sAiThreadStarted) {
		sAiThreadStarted = 1;
		sStartTb = ps3b_timebase();
		if (PS3_SOUND_CAPTURE) {
			startCapture();
		}
		ps3b_audio_start(pullOutput);
		ps3b_thread_create(aiTimerThread, NULL, 900, 0x8000, "pikmin_ai");
		ps3Log("[ps3] audio: interface timer started\n");
	}
}

// The PS3's sound output starts at boot, playing silence until the game's sound starts (AIStartDMA): a
// TV or receiver that takes a moment to pick the sound up has it by the time of the Nintendo logo's.
extern "C" void ps3AudioStartOutput(void)
{
	sOutputStartTb = ps3b_timebase();
	ps3b_audio_start(pullOutput);
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

// ---- the sub-frames, mixed alongside the game
//
// On the GameCube the DSP is a processor of its own: after mail 0 (DSPReleaseHalt) the CPU carries on while
// the DSP mixes the sub-frame, and the DSP's interrupt says when it is done. Up to test build 37 the mixing
// ran inside DSPSendMailToDSP, on the sound thread while it held the baton: 8-14% of the time in the Forest
// of Hope, with no game code running meanwhile. JAudio touches the voice blocks only between sub-frames, or
// the way it would while the real DSP was mixing, so the mixing can run elsewhere, as it does on the
// GameCube; the DSP's interrupt follows when it is done.
//
// Test build 38 mixed on a PS3 thread of its own with a higher lv2 priority than the game's threads. lv2
// ran that thread at once on the hardware thread of the sound thread that woke it, so the sound thread (with
// the baton) waited out every mix anyway and nothing was gained. Now the graphics thread mixes: it runs on
// the PPU's other hardware thread, is idle for a good part of each frame, and has the same priority as the
// game's threads (waking it does not stop the sound thread). It looks for sub-frames to mix whenever it
// waits for work and between batches of graphics commands (gx_fifo.cpp: ps3DspMixPending, ps3DspMixRun).
// PIKMIN/dsp_sync.txt: mix on the sound thread as before.
//
// Test build 39: the graphics thread started on a sub-frame 0.7 ms after it was asked for on average, but
// some waited 130-420 ms: the graphics thread's own long jobs (the minute's check frame, a recorded frame's
// picture written to the USB drive) held them up, and the sound stuttered. A rescue thread now looks every
// millisecond and mixes a sub-frame that has waited more than 2 ms itself.
//
// Test build 44: the mixing moved to an SPU (platform/ps3/spu/dsp_spu.cpp). Test build 43's log showed it at
// 250-300 us per sub-frame, 10-12% of the time, taken from the graphics thread, which limits the frame rate in
// the busiest places. Mail 0 now only counts the sub-frame in the block shared with the SPU and wakes it; when
// the SPU has mixed it, a PPU thread (ps3_spu.cpp) raises the DSP's interrupt. The rescue thread watches the
// SPU: if a sub-frame is not done after 100 ms, the SPU is stopped and the PPU mixes from then on, as before.
// PIKMIN/dsp_ppu.txt: mix on the PPU (the graphics thread) as in test build 43.
extern "C" int ps3GxThreaded(void); // gx_fifo.cpp: whether there is a graphics thread
extern "C" void ps3GxWake(void);     // gx_fifo.cpp: wakes it if it waits for work
// dsp_hle.cpp: the block shared with the SPU mixer
extern "C" u32 ps3DspSharedAddress(void);
extern "C" void ps3DspOnSpu(int on);
extern "C" void ps3DspSpuAsk(u32 asked);
extern "C" u32 ps3DspSpuDone(void);
extern "C" void ps3DspSpuLog(void);
extern "C" void ps3DspTakeOverFromSpu(u32 ofFrame);

enum { MIX_SOUND_THREAD, MIX_GRAPHICS, MIX_SPU };

static unsigned int sMixMutex;            // (held while the PPU mixes a sub-frame, or a command changes the set-up)
static bool sMixMutexOk;
static volatile u32 sMixAsked, sMixDone;  // sub-frames asked for, and mixed (their interrupts raised)
static volatile int sMixMode = -1;        // (-1: not decided yet)
static unsigned long long sMixAskedTb;    // when the last one was asked for
static unsigned long long sMixWaitTicks, sMixWaitMax; // from asked to started (PPU) or done (SPU): diagnostics, ps3AudioStatus
static u32 sMixWaits, sMixRescued;
static u32 sFrameAsked;                   // sMixAsked when the frame going on started (command 0x82)
static u32 sSpuCommandWaits;              // commands that waited for the SPU to finish a sub-frame
static u32 sSpuLateEvents;                // sub-frames the SPU finished whose interrupt the rescue thread raised

static void mixRescueThread(void*);
static void spuSubframeDone(void);

static bool dataFile(const char* name)
{
	if (!ps3b_data_root()) {
		return false;
	}
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", ps3b_data_root(), name);
	return ps3b_file_exists(path) != 0;
}

// Where the sub-frames are mixed (decided at the first DSP command).
static int mixMode()
{
	if (sMixMode < 0) {
		int mode        = MIX_SOUND_THREAD;
		const bool sync = dataFile("dsp_sync.txt");
		const bool ppu  = dataFile("dsp_ppu.txt");
		if (ps3b_mutex_create(&sMixMutex) == 0) {
			sMixMutexOk = true;
			// (without the rescue thread, a sub-frame could wait too long, or a stopped SPU go unnoticed)
			if (!sync && ps3b_thread_create(mixRescueThread, NULL, 1000, 0x10000, "pikmin_dsp_rescue") == 0) {
				mode = MIX_GRAPHICS;
				if (!ppu) {
					const char* step = "";
					const int err    = ps3b_dsp_spu_start(ps3DspSharedAddress(), spuSubframeDone, &step);
					if (err == 0) {
						mode = MIX_SPU;
						ps3DspOnSpu(1);
					} else {
						ps3Log("[ps3] audio: the SPU mixer did not start (%s: error 0x%08x)\n", step, (unsigned)err);
					}
				}
			}
		}
		sMixMode = mode;
		ps3Log("[ps3] audio: DSP sub-frames are mixed %s\n",
		       mode == MIX_SPU ? "on an SPU" : mode == MIX_GRAPHICS ? "by the graphics thread" : "on the sound thread");
	}
	return sMixMode;
}

// (graphics thread)
extern "C" int ps3DspMixPending(void) { return sMixMode == MIX_GRAPHICS && sMixDone != sMixAsked; }

// The PPU mixes the sub-frames asked for (graphics thread, rescue thread). Returns how many.
static u32 mixAsked()
{
	u32 mixed = 0;
	for (;;) {
		ps3b_mutex_lock(sMixMutex);
		if (sMixMode == MIX_SPU || sMixDone == sMixAsked) {
			ps3b_mutex_unlock(sMixMutex); // (the other mixing thread got to it first)
			return mixed;
		}
		const unsigned long long wait = ps3b_timebase() - sMixAskedTb;
		sMixWaitTicks += wait;
		sMixWaitMax = wait > sMixWaitMax ? wait : sMixWaitMax;
		sMixWaits++;
		ps3DspRenderSubframe();
		sMixDone = sMixDone + 1;
		ps3b_mutex_unlock(sMixMutex);
		ps3RaiseInterrupt(__OS_INTERRUPT_DSP_DSP);
		mixed++;
	}
}

extern "C" void ps3DspMixRun(void)
{
	if (sMixMode == MIX_GRAPHICS && sMixDone != sMixAsked) {
		mixAsked();
	}
}

// The SPU finished one or more sub-frames (its event thread; the rescue thread if that is late).
static void spuSubframeDone(void)
{
	u32 raise = 0;
	ps3b_mutex_lock(sMixMutex);
	if (sMixMode == MIX_SPU) {
		const u32 done = ps3DspSpuDone();
		if (done != sMixDone && done - sMixDone <= sMixAsked - sMixDone) {
			raise    = done - sMixDone;
			sMixDone = done;
			if (done == sMixAsked) {
				const unsigned long long wait = ps3b_timebase() - sMixAskedTb;
				sMixWaitTicks += wait;
				sMixWaitMax = wait > sMixWaitMax ? wait : sMixWaitMax;
				sMixWaits++;
			}
		}
		ps3DspSpuLog();
	}
	ps3b_mutex_unlock(sMixMutex);
	while (raise--) {
		ps3RaiseInterrupt(__OS_INTERRUPT_DSP_DSP);
	}
}

// The SPU has not finished a sub-frame for 100 ms: stop it, and mix on the PPU from here on.
static void spuGiveUp(void)
{
	u32 raise = 0;
	ps3b_mutex_lock(sMixMutex);
	if (sMixMode != MIX_SPU) {
		ps3b_mutex_unlock(sMixMutex);
		return;
	}
	const int err  = ps3b_dsp_spu_stop();
	const u32 done = ps3DspSpuDone();
	if (done - sMixDone <= sMixAsked - sMixDone) {
		raise    = done - sMixDone;
		sMixDone = done;
	}
	ps3DspSpuLog();
	ps3DspTakeOverFromSpu(sMixDone - sFrameAsked);
	sMixMode = ps3GxThreaded() ? MIX_GRAPHICS : MIX_SOUND_THREAD;
	ps3Log("[ps3] FAILED: the SPU mixer stopped answering (%u of %u sub-frames done; stopped: 0x%x); the %s mixes from now on\n",
	       (unsigned)done, (unsigned)sMixAsked, (unsigned)err, sMixMode == MIX_GRAPHICS ? "graphics thread" : "sound thread");
	ps3b_mutex_unlock(sMixMutex);
	while (raise--) {
		ps3RaiseInterrupt(__OS_INTERRUPT_DSP_DSP);
	}
	if (sMixMode == MIX_GRAPHICS) {
		ps3GxWake();
	}
}

static void mixRescueThread(void*)
{
	const unsigned long long freq = ps3b_timebase_freq();
	const unsigned long long late = freq * 2 / 1000, spuLate = freq / 10;
	for (;;) {
		ps3b_sleep_us(1000);
		if (sMixDone == sMixAsked) {
			continue;
		}
		const unsigned long long age = ps3b_timebase() - sMixAskedTb;
		if (sMixMode == MIX_SPU) {
			if (ps3DspSpuDone() == sMixAsked) {
				if (age > late) {
					sSpuLateEvents++;
					spuSubframeDone(); // (done, but its event thread has not run)
				}
			} else if (age > spuLate) {
				spuGiveUp();
			}
		} else if (age > late) {
			sMixRescued += mixAsked();
		}
	}
}

// Commands change what the SPU reads as it mixes: they wait until it has finished the sub-frames asked for.
static void waitForSpu()
{
	if (ps3DspSpuDone() == sMixAsked) {
		return;
	}
	sSpuCommandWaits++;
	for (int i = 0; i < 2000 && sMixMode == MIX_SPU && ps3DspSpuDone() != sMixAsked; i++) {
		ps3b_sleep_us(50); // (up to 100 ms; then the rescue thread takes over)
	}
}

static void runCommand()
{
	u32 id = sCmd[0] >> 24;
	if (mixMode() == MIX_SPU) {
		waitForSpu();
	}
	// (not while the PPU mixes a sub-frame)
	const bool locked = sMixMutexOk;
	if (locked) {
		ps3b_mutex_lock(sMixMutex);
	}
	if (id == 0x81) {
		ps3DspSetup(sCmd[0] & 0xFFFF, sCmd[1], sCmd[2], sCmd[3], sCmd[4]);
	}
	if (id == 0x82) {
		sSubframesLeft = (sCmd[0] >> 16) & 0xFF;
		sDspFrames++;
		sFrameAsked = sMixAsked;
		ps3DspStartFrame(sSubframesLeft, sCmd[0] & 0xFFFF, sCmd[1], sCmd[2]);
	}
	if (locked) {
		ps3b_mutex_unlock(sMixMutex);
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
				if (mixMode() == MIX_SPU) {
					ps3b_mutex_lock(sMixMutex);
					if (sMixMode == MIX_SPU) {
						sMixAskedTb = ps3b_timebase();
						sMixAsked   = sMixAsked + 1;
						ps3DspSpuAsk(sMixAsked);
						ps3b_mutex_unlock(sMixMutex);
						ps3b_dsp_spu_wake();
						return;
					}
					ps3b_mutex_unlock(sMixMutex); // (it stopped answering meanwhile)
				}
				if (sMixMode == MIX_GRAPHICS && ps3GxThreaded()) {
					ps3b_mutex_lock(sMixMutex);
					sMixAskedTb = ps3b_timebase();
					sMixAsked   = sMixAsked + 1;
					ps3b_mutex_unlock(sMixMutex);
					ps3GxWake();
				} else {
					if (sMixMutexOk) {
						ps3b_mutex_lock(sMixMutex);
					}
					ps3DspRenderSubframe();
					if (sMixMutexOk) {
						ps3b_mutex_unlock(sMixMutex);
					}
					ps3RaiseInterrupt(__OS_INTERRUPT_DSP_DSP);
				}
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
	char voices[384];
	ps3DspStatus(voices, sizeof(voices));
	char mixWait[192] = "";
	if (sMixWaits) {
		// how long sub-frames waited for the graphics thread to start on them (since the last report)
		const unsigned long long freq = ps3b_timebase_freq();
		const u32 n = sMixWaits;
		const unsigned avg = n && freq ? (unsigned)(sMixWaitTicks * 1000000ULL / freq / n) : 0u;
		const unsigned max = freq ? (unsigned)(sMixWaitMax * 1000000ULL / freq) : 0u;
		if (sMixMode == MIX_SPU) {
			// (from mail 0 to the interrupt)
			snprintf(mixWait, sizeof(mixWait), ", each done by the SPU %u us after it was asked for on average (longest %u us; %u commands waited, %u late)",
			         avg, max, (unsigned)sSpuCommandWaits, (unsigned)sSpuLateEvents);
		} else {
			snprintf(mixWait, sizeof(mixWait), ", mixed by the graphics thread after %u us on average (longest %u us; %u by the rescue thread)", avg, max,
			         (unsigned)sMixRescued);
		}
		sMixWaitTicks = 0;
		sMixWaitMax   = 0;
		sMixWaits     = 0;
		sMixRescued   = 0;
	}

	// The music/movie stream (jaudio/syncstream.c): what a cutscene waits for.
	static const char* const kStates[] = { "starting", "playing", "ready", "stopping", "idle", "buffering", "paused" };
	const StreamCtrl_* sc = ps3StreamControl;
	u32 state             = sc->playbackState;
	u32 id                = sc->streamId;
	u32 loaded            = sc->samplesLoaded;
	u32 total             = sc->totalSamples;
	char stream[96];
	if (state == 4) {
		snprintf(stream, sizeof(stream), "stream idle (%u stalls)", (unsigned)sStreamStalls);
	} else {
		snprintf(stream, sizeof(stream), "stream %d %s (%u of %u samples, %u stalls)", (int)id, state < 7 ? kStates[state] : "?",
		         (unsigned)loaded, (unsigned)total, (unsigned)sStreamStalls);
	}
	char output[160];
	if (outputRunning(ps3b_timebase(), ps3b_timebase_freq())) {
		snprintf(output, sizeof(output), "sound out: %u queued, %u gaps, %u repeats, %u late blocks (longest wait %u ms)",
		         (unsigned)(sRingWrite - sRingRead), (unsigned)sUnderruns, (unsigned)sRepeats, (unsigned)sLateBlocks,
		         (unsigned)(sLateMaxUs / 1000));
		sLateMaxUs = 0;
	} else {
		snprintf(output, sizeof(output), "sound out: off, %u repeats", (unsigned)sRepeats);
	}
	snprintf(buf, size, "audio: %u AI blocks, %u DSP frames, %u DSP syncs, %s%s, %u overload checks; %s; %s", (unsigned)sAiBlocks,
	         (unsigned)sDspFrames, (unsigned)sDspSyncs, voices, mixWait, (unsigned)sDspOverloads, stream, output);
}
