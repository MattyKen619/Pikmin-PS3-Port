// PS3 port: the audio DSP's mixing microcode, reimplemented on the PPU.
//
// JAudio describes every playing sound as a 0x180-byte voice block (DSPchannel_ in
// jaudio/dspinterface.h) in main memory. On the GameCube, the DSP goes through the voice blocks once
// per sub-frame (80 samples, 2.5 ms). For each voice it decodes samples (AFC/ADPCM or PCM from ARAM,
// PCM streamed from main memory, or a built-in oscillator), resamples them to the output rate, and
// mixes them into its buses with volume ramps. Then it writes the voice's progress back: position,
// remaining length, and the "done" flag when the sound has ended or a requested stop has faded out.
//
// JAudio frees voices and advances music and movie streams from that progress, so the game needs it
// even before there is any sound output. Without it, a stopped stream never finishes stopping, and the
// next cutscene waits for it forever (test 7).
//
// The behavior follows how the Dolphin emulator documents this microcode family (the "Zelda"/DAC
// microcode, light protocol, as used by Pikmin); Dolphin also named the voice block fields. This file
// is a separate implementation. Differences: buses use 32-bit accumulators (saturated when stored),
// volume ramps end exactly at their target, and a few guards stop bad voice data from reading outside
// ARAM or looping forever. These make sure a voice that is asked to stop always finishes.
//
// Everything here runs on the audio thread, inside DSPSendMailToDSP(), while it holds the scheduler's
// baton, so no other game code touches the voice blocks meanwhile.

#include "sdk_internal.h"
#include "../backend/ps3_backend.h"

#include "jaudio/dspinterface.h"
#include <stdio.h>
#include <string.h>

extern "C" u8* ps3AramRange(u32 address, u32 size); // vi_pad_ar_card.cpp

// The voice block layout is fixed by the microcode: check the fields this file uses. (MSL's offsetof
// is not a constant expression for GCC, hence the builtin.)
#define CHECK_OFFSET(field, offset) static_assert(__builtin_offsetof(DSPchannel_, field) == (offset), "voice block layout: " #field)
CHECK_OFFSET(enabled, 0x00);
CHECK_OFFSET(done, 0x02);
CHECK_OFFSET(resamplingRatio, 0x04);
CHECK_OFFSET(resetVpb, 0x08);
CHECK_OFFSET(endReached, 0x0A);
CHECK_OFFSET(useConstantSample, 0x0C);
CHECK_OFFSET(mixChannels, 0x10);
CHECK_OFFSET(dolbyVolumeCurrent, 0x54);
CHECK_OFFSET(dolbyVolumeTarget, 0x56);
CHECK_OFFSET(useDolbyVolume, 0x58);
CHECK_OFFSET(currentPosFrac, 0x60);
CHECK_OFFSET(afcRemainingDecodedSamples, 0x64);
CHECK_OFFSET(constantSample, 0x66);
CHECK_OFFSET(currentPosition, 0x68);
CHECK_OFFSET(samplesBeforeLoop, 0x6C);
CHECK_OFFSET(currentAramAddr, 0x70);
CHECK_OFFSET(remainingLength, 0x74);
CHECK_OFFSET(resampleBuffer, 0x78);
CHECK_OFFSET(biquadHistory, 0xA8);
CHECK_OFFSET(afcRemainingSamples, 0xB0);
CHECK_OFFSET(lowPassHistory, 0xD0);
CHECK_OFFSET(samplesSourceType, 0x100);
CHECK_OFFSET(isLooping, 0x102);
CHECK_OFFSET(loopYN1, 0x104);
CHECK_OFFSET(loopYN2, 0x106);
CHECK_OFFSET(filterMode, 0x108);
CHECK_OFFSET(endRequested, 0x10A);
CHECK_OFFSET(loopAddress, 0x110);
CHECK_OFFSET(loopStartPosition, 0x114);
CHECK_OFFSET(baseAddress, 0x118);
CHECK_OFFSET(biquadFilterCoeffs, 0x148);
CHECK_OFFSET(lowPassCoeff, 0x150);
static_assert(sizeof(DSPchannel_) == 0x180, "voice block size");
#undef CHECK_OFFSET

namespace {

enum {
	TRACKED_VOICES = 128,
	SUBFRAME     = 0x50,  // samples per sub-frame
	VOICE_RW     = 0x100, // the DSP writes back only the first 0x100 bytes of a voice block
	MAX_RAW      = 4 + 0x500 + 0x10,
	PATTERN_SIZE = 0x40,
};

// Sample sources (DSPchannel_::samplesSourceType)
enum {
	SRC_SQUARE         = 0,
	SRC_SAW            = 1,
	SRC_SQUARE_25      = 3,
	SRC_PATTERN_1      = 4,
	SRC_AFC_LQ_ARAM    = 5, // 16 samples in 5 bytes
	SRC_PATTERN_0      = 7,
	SRC_PCM8_ARAM      = 8,
	SRC_AFC_HQ_ARAM    = 9, // 16 samples in 9 bytes
	SRC_PATTERN_0_VAR  = 10,
	SRC_PATTERN_2      = 11,
	SRC_PATTERN_3      = 12,
	SRC_PCM16_ARAM     = 16,
	SRC_PCM16_MRAM     = 33, // streamed audio (movies, cutscenes)
};

struct Bus {
	u16 id; // the DSP memory address JAudio uses to name the bus (DSP_SetBusConnect)
	s32 s[SUBFRAME];
};

enum {
	BUS_FRONT_L,
	BUS_FRONT_R,
	BUS_BACK_L,
	BUS_BACK_R,
	BUS_FRONT_L_REVERB,
	BUS_FRONT_R_REVERB,
	BUS_BACK_L_REVERB,
	BUS_BACK_R_REVERB,
	BUS_REVERB_0,
	BUS_REVERB_1,
	BUS_EXTRA_0,
	BUS_EXTRA_1,
	BUS_EXTRA_2,
	BUS_COUNT
};

static Bus sBus[BUS_COUNT] = {
	{ 0x0D00 }, { 0x0D60 }, { 0x0F40 }, { 0x0CA0 }, { 0x0E80 }, { 0x0EE0 }, { 0x0C00 },
	{ 0x0C50 }, { 0x0DC0 }, { 0x0E20 }, { 0x09A0 }, { 0x0FA0 }, { 0x0B00 },
};

// The 4 reverb lines (FXBuffer) feed these buses.
static const int kReverbBus[4] = { BUS_REVERB_0, BUS_REVERB_1, BUS_FRONT_L_REVERB, BUS_FRONT_R_REVERB };

struct State {
	bool configured;
	u32 voiceCount;
	DSPchannel_* voices;
	FXBuffer* reverb;
	s16 resampleCoeffs[0x100];
	s16 patterns[0x100];
	s16 afcCoeffs[0x20];

	u16 outputVolume; // 4.12
	s16* outL;
	s16* outR;
	u32 outLeft; // sub-frames the CPU's output buffers still have room for

	u16 reverbPos[4];
	s16 reverbLast8[4][8];

	// diagnostics
	u32 subframes;
	unsigned long long mixTicks; // timebase ticks spent mixing (ps3DspRenderSubframe)
	unsigned long long sourceTicks, filterTicks, mixVoiceTicks; // ... of which: reading and resampling the voices'
	                                                            // samples, their filters, mixing them into the buses
	u32 voicesMixed;             // voices processed (all sub-frames)
	u32 playing;   // voices processed in the last sub-frame
	u32 finished;  // voices that reached "done"
	u32 badVoices; // voices stopped because their data was out of range
	u32 aramOverwrites; // voices playing when ARAM under their sound data was written (ps3DspNoteAramWrite)
	u32 loggedAramOverwrites;
	volatile u8 stopRequested[TRACKED_VOICES]; // (ps3DspNoteAramWrite: stopped at the next sub-frame)
	u32 loggedStarts;
	u32 loggedStops;
	// per voice, for the start/stop lines of the first sounds of a session
	u8 active[TRACKED_VOICES];      // playing in the previous sub-frame
	u8 fadeRequested[TRACKED_VOICES];
	u8 endedHere[TRACKED_VOICES];   // this file set "done" (sound ended, or a requested fade finished)
	u32 startedAt[TRACKED_VOICES];  // sub-frame count when it started
	u16 peakIn[TRACKED_VOICES];     // largest input sample (before volume) since it started: 0 = silent data
};

static State S;

static inline s16 clamp16(s64 v)
{
	return (s16)(v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v));
}

static inline s16 clamp16(s32 v)
{
	return (s16)(v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v));
}

// (A note on speed: the PPU multiplies integers slowly, 64-bit ones more slowly still, so products of
// 16-bit values are made as 32-bit ones (they fit) and only their sums are 64-bit where they could overflow.)

// The bus a mixer channel names: by bits 4-11 of its id, which differ for every bus (then checked).
static Bus* sBusById[256];
static bool sBusTable;

static Bus* busFor(u16 id)
{
	if (id == 0) {
		return NULL;
	}
	if (!sBusTable) {
		sBusTable = true;
		for (int i = 0; i < BUS_COUNT; i++) {
			sBusById[(sBus[i].id >> 4) & 0xFF] = &sBus[i];
		}
	}
	Bus* b = sBusById[(id >> 4) & 0xFF];
	if (b && b->id == id) {
		return b;
	}
	static int sLogged;
	ps3LogOnce(&sLogged, "[ps3] audio: voice mixes to unknown DSP bus 0x%04x (ignored)\n", (unsigned)id);
	return NULL;
}

static void badVoice(DSPchannel_* v, const char* what, u32 address, u32 size)
{
	static int sLogged;
	ps3LogOnce(&sLogged, "[ps3] audio: %s out of range (0x%08x + 0x%x, source %u): voice stopped\n", what, (unsigned)address,
	           (unsigned)size, (unsigned)v->samplesSourceType);
	v->done = DSP_TRUE;
	S.badVoices++;
}

// ------------------------------------------------------------------ sample sources

static inline u16 neededRawSamples(const DSPchannel_* v)
{
	// position fraction and ratio are 4.12 fixed point
	return (u16)((v->currentPosFrac + SUBFRAME * (u32)v->resamplingRatio) >> 12);
}

static void resample(DSPchannel_* v, const s16* src, s16* dst)
{
	u32 ratio = v->resamplingRatio;
	u32 pos   = v->currentPosFrac;
	if ((ratio >> 12) >= 4) {
		// more than 4:1: no interpolation
		for (int i = 0; i < SUBFRAME; i++) {
			pos += ratio;
			dst[i] = src[pos >> 12];
		}
	} else {
		const s16* coeffs = S.resampleCoeffs;
		for (int i = 0; i < SUBFRAME; i++) {
			// 4-tap filter; the top 6 bits of the fraction select the coefficients. (The microcode doubles the
			// sum and keeps its top 16 bits: the same as the sum shifted by 15.)
			const s16* c  = &coeffs[((pos & 0xFFF) >> 6) * 4];
			const s16* in = &src[pos >> 12];
			const s64 acc = (s64)((s32)c[0] * in[0]) + (s64)((s32)c[1] * in[1]) + (s64)((s32)c[2] * in[2]) + (s64)((s32)c[3] * in[3]);
			dst[i]        = clamp16(acc >> 15);
			pos += ratio;
		}
	}
	for (int k = 0; k < 4; k++) {
		v->resampleBuffer[k] = src[(pos >> 12) + k];
	}
	v->constantSample = dst[SUBFRAME - 1];
	v->currentPosFrac = pos & 0xFFF;
}

// PCM8 / PCM16 from ARAM (big-endian samples, as loaded from the disc)
static void pcmFromAram(DSPchannel_* v, s16* dst, u32 count, u32 bytesPerSample)
{
	if (v->resetVpb) {
		v->remainingLength = v->loopStartPosition - v->currentPosition;
		v->currentAramAddr = v->baseAddress + v->currentPosition * bytesPerSample;
	}
	v->endReached = DSP_FALSE;
	int guard     = 0;
	while (count) {
		if (v->endReached) {
			v->endReached = DSP_FALSE;
			if (!v->isLooping || ++guard > 64) {
				memset(dst, 0, count * sizeof(s16));
				v->done = DSP_TRUE;
				return;
			}
			v->currentPosition = v->loopAddress;
			v->remainingLength = v->loopStartPosition - v->currentPosition;
			v->currentAramAddr = v->baseAddress + v->currentPosition * bytesPerSample;
		}
		u32 n = v->remainingLength < count ? v->remainingLength : count;
		if (n) {
			const u8* p = ps3AramRange(v->currentAramAddr, n * bytesPerSample);
			if (!p) {
				badVoice(v, "PCM sound data in ARAM", v->currentAramAddr, n * bytesPerSample);
				memset(dst, 0, count * sizeof(s16));
				return;
			}
			if (bytesPerSample == 1) {
				for (u32 i = 0; i < n; i++) {
					dst[i] = (s16)((u16)p[i] << 8);
				}
			} else {
				for (u32 i = 0; i < n; i++) {
					dst[i] = (s16)((p[2 * i] << 8) | p[2 * i + 1]);
				}
			}
		}
		v->remainingLength -= n;
		v->currentAramAddr += n * bytesPerSample;
		dst += n;
		count -= n;
		if (v->remainingLength == 0) {
			v->endReached = DSP_TRUE;
		}
	}
}

// AFC (ADPCM variant) blocks of 16 samples: one header byte (scale, predictor), then 4-bit (9-byte
// blocks) or 2-bit (5-byte blocks) deltas.
static bool decodeAfc(DSPchannel_* v, s16* dst, u32 blocks)
{
	u32 blockBytes = v->samplesSourceType; // 9 or 5
	const u8* src  = ps3AramRange(v->currentAramAddr, blocks * blockBytes);
	if (!src) {
		badVoice(v, "AFC sound data in ARAM", v->currentAramAddr, blocks * blockBytes);
		memset(dst, 0, blocks * 16 * sizeof(s16));
		return false;
	}
	v->currentAramAddr += blocks * blockBytes;

	s16* hist = (s16*)v->afcRemainingSamples; // [14] = yn2, [15] = yn1
	for (u32 b = 0; b < blocks; b++) {
		const u32 shift = (*src >> 4) & 0xF; // (the scale is 1 << shift)
		const u32 pred  = *src & 0xF;
		const s32 c0    = S.afcCoeffs[pred * 2];
		const s32 c1    = S.afcCoeffs[pred * 2 + 1];
		src++;
		s16 delta[16];
		if (blockBytes == SRC_AFC_HQ_ARAM) {
			for (int i = 0; i < 16; i += 2) {
				delta[i]     = (s16)((u16)(*src >> 4) << 12) >> 1;
				delta[i + 1] = (s16)((u16)(*src & 0xF) << 12) >> 1;
				src++;
			}
		} else {
			for (int i = 0; i < 16; i += 4) {
				delta[i]     = (s16)((u16)((*src >> 6) & 3) << 14) >> 1;
				delta[i + 1] = (s16)((u16)((*src >> 4) & 3) << 14) >> 1;
				delta[i + 2] = (s16)((u16)((*src >> 2) & 3) << 14) >> 1;
				delta[i + 3] = (s16)((u16)(*src & 3) << 14) >> 1;
				src++;
			}
		}
		s32 yn1 = hist[15];
		s32 yn2 = hist[14];
		for (int i = 0; i < 16; i++) {
			const s64 acc = (s64)(s32)((u32)(s32)delta[i] << shift) + (s64)(yn1 * c0) + (s64)(yn2 * c1);
			s32 sample    = clamp16(acc >> 11);
			*dst++     = (s16)sample;
			yn2        = yn1;
			yn1        = sample;
		}
		hist[14] = (s16)yn2;
		hist[15] = (s16)yn1;
	}
	return true;
}

static void afcFromAram(DSPchannel_* v, s16* dst, u32 count)
{
	s16* cache = (s16*)v->afcRemainingSamples; // the last decoded block; its tail is not output yet
	if (v->resetVpb) {
		cache[14]                     = 0;
		cache[15]                     = 0;
		v->afcRemainingDecodedSamples = 0;
		v->remainingLength            = v->loopStartPosition; // total length in samples
		v->currentAramAddr            = v->baseAddress;
	}
	if (v->afcRemainingDecodedSamples > 16) {
		v->afcRemainingDecodedSamples = 16;
	}

	for (int guard = 0; guard < 64; guard++) {
		// first, samples already decoded
		u32 n         = v->afcRemainingDecodedSamples < count ? v->afcRemainingDecodedSamples : count;
		const s16* in = &cache[16 - v->afcRemainingDecodedSamples];
		for (u32 i = 0; i < n; i++) {
			*dst++ = in[i];
		}
		v->afcRemainingDecodedSamples -= n;
		count -= n;
		if (count == 0) {
			return;
		}

		if (count <= v->remainingLength) {
			u32 blocks  = (count + 15) >> 4;
			u32 decoded = blocks << 4;
			if (decoded < v->remainingLength) {
				v->afcRemainingDecodedSamples = decoded - count;
				v->remainingLength -= decoded;
			} else {
				v->afcRemainingDecodedSamples = v->remainingLength - count;
				v->remainingLength            = 0;
			}
			if (!decodeAfc(v, dst, blocks)) {
				return;
			}
			if (v->afcRemainingDecodedSamples) {
				for (int i = 0; i < 16; i++) {
					cache[i] = dst[decoded - 16 + i];
				}
				if (v->remainingLength == 0 && v->loopStartPosition) {
					// the sound ends inside this block: move its last samples to the end of the cache
					int top = (int)((v->loopStartPosition + 0xF) & 0xF);
					for (u32 i = 0; i < v->afcRemainingDecodedSamples && top - (int)i >= 0; i++) {
						cache[15 - i] = cache[top - i];
					}
				}
			}
			return;
		}

		// asked for more than is left: finish the sound, or loop
		if (v->remainingLength) {
			count -= v->remainingLength;
			if (!decodeAfc(v, dst, (v->remainingLength + 15) >> 4)) {
				return;
			}
			dst += v->remainingLength;
		}
		if (!v->isLooping) {
			v->done = DSP_TRUE;
			memset(dst, 0, count * sizeof(s16));
			return;
		}
		// loop: loopAddress is the loop start in samples; source type = bytes per 16 samples
		v->currentAramAddr = v->baseAddress + (v->loopAddress >> 4) * v->samplesSourceType;
		cache[14]          = v->loopYN2;
		cache[15]          = v->loopYN1;
		if (!decodeAfc(v, cache, 1)) {
			return;
		}
		v->afcRemainingDecodedSamples = 16 - (v->loopAddress & 0xF);
		v->remainingLength            = v->loopStartPosition - v->afcRemainingDecodedSamples - v->loopAddress;
	}
	badVoice(v, "AFC loop", v->loopAddress, v->loopStartPosition);
	memset(dst, 0, count * sizeof(s16));
}

// PCM16 in main memory, read in a circular buffer: how the CPU streams movie and cutscene audio.
static void pcm16FromMram(DSPchannel_* v, s16* dst, u32 count)
{
	u16 pos     = (u16)(v->currentPosition >> 16);
	u32 remain  = v->remainingLength;
	if (v->baseAddress == 0) {
		badVoice(v, "streamed sound buffer", 0, count * 2);
		memset(dst, 0, count * sizeof(s16));
		return;
	}
	const s16* src = (const s16*)(uptr)v->baseAddress + pos;
	if (count > remain) {
		s16 last = 0;
		for (u32 i = 0; i < remain; i++) {
			dst[i] = last = src[i];
		}
		for (u32 i = remain; i < count; i++) {
			dst[i] = last;
		}
		pos += (u16)remain;
		v->remainingLength = 0;
		v->done            = DSP_TRUE;
	} else {
		v->remainingLength = remain - count;
		u16 loopEnd        = (u16)(v->loopStartPosition >> 16);
		v->samplesBeforeLoop = (u16)(loopEnd - pos);
		if (count <= v->samplesBeforeLoop) {
			for (u32 i = 0; i < count; i++) {
				dst[i] = src[i];
			}
			pos += (u16)count;
		} else {
			u32 first = v->samplesBeforeLoop;
			for (u32 i = 0; i < first; i++) {
				dst[i] = src[i];
			}
			v->baseAddress = v->loopAddress;
			src            = (const s16*)(uptr)v->loopAddress;
			pos            = (u16)(count - first);
			for (u32 i = 0; i < pos; i++) {
				dst[first + i] = src[i];
			}
		}
	}
	v->currentPosition = ((u32)pos << 16) | (v->currentPosition & 0xFFFF);
}

static void oscillator(DSPchannel_* v, s16* out)
{
	switch (v->samplesSourceType) {
	case SRC_SQUARE:
	case SRC_SQUARE_25:
	{
		u32 shift = v->samplesSourceType == SRC_SQUARE ? 1 : 2;
		u32 mask  = (1u << shift) - 1;
		u32 ratio = (u32)v->resamplingRatio << (shift - 1);
		u32 pos   = (u32)v->currentPosFrac << shift;
		for (int i = 0; i < SUBFRAME; i++) {
			out[i] = ((pos >> 16) & mask) ? (s16)0xC000 : (s16)0x4000;
			pos += ratio;
		}
		v->currentPosFrac = (u16)(pos >> shift);
		break;
	}
	case SRC_SAW:
	{
		u32 pos = v->currentPosFrac;
		for (int i = 0; i < SUBFRAME; i++) {
			out[i] = (s16)(u16)pos;
			pos += v->resamplingRatio >> 1;
		}
		v->currentPosFrac = (u16)pos;
		break;
	}
	default:
	{
		int pattern;
		bool variable = false;
		switch (v->samplesSourceType) {
		case SRC_PATTERN_0: pattern = 0; break;
		case SRC_PATTERN_0_VAR: pattern = 0; variable = true; break;
		case SRC_PATTERN_1: pattern = 1; break;
		case SRC_PATTERN_2: pattern = 2; break;
		default: pattern = 3; break;
		}
		const s16* p = &S.patterns[pattern * PATTERN_SIZE];
		u32 pos      = (u32)v->currentPosFrac << 6;
		u32 step     = (u32)v->resamplingRatio << 5;
		for (int i = 0; i < SUBFRAME; i++) {
			out[i] = p[(pos >> 16) & (PATTERN_SIZE - 1)];
			pos    = (pos + step) % (PATTERN_SIZE << 16);
			if (variable) {
				s32 mod = clamp16(sBus[BUS_BACK_R].s[i]);
				pos     = ((pos << 10) + (u32)(mod * (s32)v->resamplingRatio)) >> 10;
			}
		}
		v->currentPosFrac = (u16)(pos >> 6);
		break;
	}
	}
}

static void loadInput(DSPchannel_* v, s16* out)
{
	if (v->useConstantSample) {
		// paused, or being silenced
		for (int i = 0; i < SUBFRAME; i++) {
			out[i] = v->constantSample;
		}
		return;
	}

	s16 raw[MAX_RAW];
	for (int k = 0; k < 4; k++) {
		raw[k] = v->resampleBuffer[k];
	}
	u32 need = neededRawSamples(v);

	switch (v->samplesSourceType) {
	case SRC_SQUARE:
	case SRC_SAW:
	case SRC_SQUARE_25:
	case SRC_PATTERN_0:
	case SRC_PATTERN_0_VAR:
	case SRC_PATTERN_1:
	case SRC_PATTERN_2:
	case SRC_PATTERN_3:
		oscillator(v, out);
		return;
	case SRC_PCM8_ARAM:
		pcmFromAram(v, raw + 4, need, 1);
		break;
	case SRC_PCM16_ARAM:
		pcmFromAram(v, raw + 4, need, 2);
		break;
	case SRC_AFC_HQ_ARAM:
	case SRC_AFC_LQ_ARAM:
		afcFromAram(v, raw + 4, need);
		break;
	case SRC_PCM16_MRAM:
		pcm16FromMram(v, raw + 4, need);
		break;
	default:
	{
		static u32 sLoggedTypes;
		u32 bit = 1u << (v->samplesSourceType & 31);
		if (!(sLoggedTypes & bit)) {
			sLoggedTypes |= bit;
			ps3Log("[ps3] audio: unknown sound source type %u (silent)\n", (unsigned)v->samplesSourceType);
		}
		memset(out, 0, SUBFRAME * sizeof(s16));
		return;
	}
	}
	resample(v, raw, out);
}

// ------------------------------------------------------------------ filters and mixing

static void lowPass(DSPchannel_* v, s16* buf)
{
	s32 yn1   = v->resetVpb ? 0 : v->lowPassHistory[0];
	s32 xn1   = v->resetVpb ? 0 : v->lowPassHistory[1];
	s32 coeff = (u16)v->lowPassCoeff;
	for (int i = 0; i < SUBFRAME; i++) {
		s32 xn0 = buf[i];
		s64 t   = (((s64)xn0 - xn1) * coeff >> 7) + yn1;
		s16 yn0 = clamp16(t);
		buf[i]  = yn0;
		yn1     = yn0;
		xn1     = xn0;
	}
	v->lowPassHistory[0] = (s16)yn1;
	v->lowPassHistory[1] = (s16)xn1;
}

static void biquad(DSPchannel_* v, s16* buf)
{
	s16* h       = v->biquadHistory; // xn1, xn2, yn1, yn2
	const s16* c = v->biquadFilterCoeffs; // bn1, bn2, an1, an2
	s32 xn1 = h[0], xn2 = h[1], yn1 = h[2], yn2 = h[3];
	const s32 c0 = c[0], c1 = c[1], c2 = c[2], c3 = c[3];
	for (int i = 0; i < SUBFRAME; i++) {
		s32 xn0 = buf[i];
		s64 t   = (s64)(c0 * xn1) + (s64)(c1 * xn2) + (s64)(c2 * yn1) + (s64)(c3 * yn2);
		s16 yn0 = clamp16(t >> 15);
		buf[i]  = yn0;
		xn2     = xn1;
		xn1     = xn0;
		yn2     = yn1;
		yn1     = yn0;
	}
	h[0] = (s16)xn1;
	h[1] = (s16)xn2;
	h[2] = (s16)yn1;
	h[3] = (s16)yn2;
}

// Mix with a volume ramp; volumes are 1.15, the ramp is in 1.31.
static void mixRamp(Bus* bus, const s16* src, s32 vol, s32 step)
{
	s32* out = bus->s;
	if (step == 0) {
		const s32 v = vol >> 16;
		for (int i = 0; i < SUBFRAME; i += 4) {
			out[i] += (v * src[i]) >> 16;
			out[i + 1] += (v * src[i + 1]) >> 16;
			out[i + 2] += (v * src[i + 2]) >> 16;
			out[i + 3] += (v * src[i + 3]) >> 16;
		}
		return;
	}
	for (int i = 0; i < SUBFRAME; i++) {
		out[i] += ((vol >> 16) * src[i]) >> 16;
		vol += step;
	}
}

static void addWithVolume(Bus* bus, const s16* src, int count, int offset, u16 vol)
{
	for (int i = 0; i < count; i++) {
		bus->s[offset + i] += clamp16(((s32)src[i] * (s32)vol) >> 15);
	}
}

static void renderVoice(DSPchannel_* block)
{
	const u32 slot = (u32)(block - S.voices);
	if (slot < TRACKED_VOICES && S.stopRequested[slot]) {
		// (its sound data in ARAM was replaced: ps3DspNoteAramWrite)
		S.stopRequested[slot] = 0;
		if (block->enabled && !block->done) {
			block->done = DSP_TRUE;
			S.finished++;
		}
		return;
	}
	if (!block->enabled || block->done) {
		return;
	}
	DSPchannel_ v;
	memcpy(&v, block, sizeof(v));
	S.playing++;
	const u32 index = (u32)(block - S.voices);
	const bool tracked = index < TRACKED_VOICES;
	if (v.resetVpb && tracked) {
		S.startedAt[index]     = S.subframes;
		S.fadeRequested[index] = 0;
		S.endedHere[index]     = 0;
		S.peakIn[index]        = 0;
	}
	if (v.endRequested && tracked && !S.fadeRequested[index]) {
		S.fadeRequested[index] = 1;
		if (S.loggedStops < 24) {
			ps3Log("[sound] voice %d: the game asks it to stop, after %u ms\n", (int)index,
			       (unsigned)((S.subframes - S.startedAt[index]) * 5 / 2));
		}
	}
	if (v.resetVpb && S.loggedStarts < 24) {
		// The first sounds of a session: shows whether music and effects start, and with what data.
		S.loggedStarts++;
		ps3Log("[sound] voice %d starts: source %u at 0x%08x, length %u, loop %s at %u, pitch 0x%04x, volume %d/%d\n",
		       (int)(block - S.voices), (unsigned)v.samplesSourceType, (unsigned)v.baseAddress, (unsigned)v.loopStartPosition,
		       v.isLooping ? "on" : "off", (unsigned)v.loopAddress, (unsigned)v.resamplingRatio, (int)(s16)v.mixChannels[0].targetVolume,
		       (int)(s16)v.mixChannels[1].targetVolume);
	}

	s16 in[SUBFRAME];
	const unsigned long long t0 = ps3b_timebase();
	loadInput(&v, in);
	const unsigned long long t1 = ps3b_timebase();
	S.sourceTicks += t1 - t0;
	if (tracked && S.loggedStops < 24) {
		u16 pk = S.peakIn[index];
		for (int i = 0; i < SUBFRAME; i++) {
			const u16 a = (u16)(in[i] < 0 ? -(s32)in[i] : in[i]);
			pk          = a > pk ? a : pk;
		}
		S.peakIn[index] = pk;
	}

	if (v.lowPassCoeff != 0) {
		lowPass(&v, in);
	}
	if ((v.filterMode & 0x20)
	    && (v.biquadFilterCoeffs[0] != 0x7FFF || v.biquadFilterCoeffs[1] != 0 || v.biquadFilterCoeffs[2] != 0
	        || v.biquadFilterCoeffs[3] != 0)) {
		biquad(&v, in);
	}

	const unsigned long long t2 = ps3b_timebase();
	S.filterTicks += t2 - t1;
	if (v.useDolbyVolume) {
		// Positional (Dolby) mixing is not part of the light microcode: silent, but stops correctly.
		static int sLogged;
		ps3LogOnce(&sLogged, "[ps3] audio: a voice uses surround positioning (not supported, silent)\n");
		if (v.endRequested) {
			v.dolbyVolumeTarget = v.dolbyVolumeCurrent / 2;
			if (v.dolbyVolumeTarget == 0) {
				v.done = DSP_TRUE;
			}
		}
		v.dolbyVolumeCurrent = v.dolbyVolumeTarget;
	} else {
		if (v.endRequested) {
			// fade out quickly: halve the volume every sub-frame, done at silence
			bool silent = true;
			for (int c = 0; c < 6; c++) {
				DSPMixerChannel* ch = &v.mixChannels[c];
				ch->targetVolume    = (u16)((s16)ch->currentVolume / 2);
				silent &= (ch->targetVolume == 0);
			}
			if (silent) {
				v.done = DSP_TRUE;
			}
		}
		for (int c = 0; c < 6; c++) {
			DSPMixerChannel* ch = &v.mixChannels[c];
			s16 delta           = (s16)((s16)ch->targetVolume - (s16)ch->currentVolume);
			s32 step            = ((s32)delta * 65536) / SUBFRAME;
			Bus* bus            = ch->id ? busFor(ch->id) : NULL;
			if (bus && (ch->currentVolume || step)) {
				mixRamp(bus, in, (s32)(s16)ch->currentVolume * 65536, step);
			}
			// The ramp ends at the target, also for channels not connected to a bus. (Keeping the ramp's
			// last value instead can stop one step short forever because of rounding, and then a fading
			// voice never finishes.)
			ch->currentVolume = ch->targetVolume;
		}
	}
	S.mixVoiceTicks += ps3b_timebase() - t2;

	if (!v.useConstantSample) {
		v.resetVpb = DSP_FALSE;
	}
	if (v.done) {
		S.finished++;
		if (S.finished == 1) {
			ps3Log("[ps3] audio: DSP voice engine working (first sound finished)\n");
		}
		if (tracked) {
			S.endedHere[index] = 1;
		}
	}
	memcpy(block, &v, VOICE_RW);
}

// ------------------------------------------------------------------ reverb

static void filter8(s16* buf, const s16* coeffs)
{
	// buf has 8 samples of history before the 0x50 new ones
	for (int i = 0; i < SUBFRAME; i++) {
		s32 acc = 0;
		for (int j = 0; j < 8; j++) {
			acc += (s32)buf[i + j] * coeffs[j];
		}
		buf[i] = clamp16(acc >> 15);
	}
}

static void reverbBefore()
{
	if (!S.reverb) {
		return;
	}
	for (int r = 0; r < 4; r++) {
		FXBuffer* fx = &S.reverb[r];
		Bus* line    = &sBus[kReverbBus[r]];
		if (!fx->enabled || !fx->circularBufferBase || !fx->circularBufferSize) {
			memset(line->s, 0, sizeof(line->s));
			continue;
		}
		if (S.reverbPos[r] >= fx->circularBufferSize) {
			S.reverbPos[r] = 0;
		}
		const s16* mem = fx->circularBufferBase + S.reverbPos[r] * SUBFRAME;
		s16 buf[SUBFRAME + 8];
		memcpy(buf, S.reverbLast8[r], 8 * sizeof(s16));
		memcpy(buf + 8, mem, SUBFRAME * sizeof(s16));
		memcpy(S.reverbLast8[r], buf + SUBFRAME, 8 * sizeof(s16));

		if (fx->enabled & 1) {
			filter8(buf, fx->filterCoeffs);
		}
		for (int d = 0; d < 2; d++) {
			Bus* dest = busFor(fx->dest[d].bufferId);
			if (dest) {
				addWithVolume(dest, buf, SUBFRAME, 0, (u16)fx->dest[d].volume);
			}
		}
		if (fx->enabled & 2) {
			filter8(buf, fx->filterCoeffs);
		}
		for (int i = 0; i < SUBFRAME; i++) {
			line->s[i] = buf[i];
		}
	}
}

static void reverbAfter()
{
	if (!S.reverb) {
		return;
	}
	for (int r = 0; r < 4; r++) {
		FXBuffer* fx = &S.reverb[r];
		if (!fx->enabled || !fx->circularBufferBase || !fx->circularBufferSize) {
			continue;
		}
		s16* mem  = fx->circularBufferBase + S.reverbPos[r] * SUBFRAME;
		Bus* line = &sBus[kReverbBus[r]];
		for (int i = 0; i < SUBFRAME; i++) {
			mem[i] = clamp16(line->s[i]);
		}
		S.reverbPos[r] = (u16)((S.reverbPos[r] + 1) % fx->circularBufferSize);
	}
}

// ------------------------------------------------------------------ sub-frame

static void prepareSubframe()
{
	memset(sBus[BUS_FRONT_L].s, 0, sizeof(sBus[0].s));
	memset(sBus[BUS_FRONT_R].s, 0, sizeof(sBus[0].s));
	for (int i = 0; i < SUBFRAME; i++) {
		// the back buses decay instead of being cleared
		sBus[BUS_BACK_L].s[i] = clamp16(((s32)clamp16(sBus[BUS_BACK_L].s[i]) * 0x6784) >> 15);
		sBus[BUS_BACK_R].s[i] = clamp16(((s32)clamp16(sBus[BUS_BACK_R].s[i]) * 0x6784) >> 15);
	}
	// buses nothing reads back: keep them from growing
	memset(sBus[BUS_EXTRA_0].s, 0, sizeof(sBus[0].s));
	memset(sBus[BUS_EXTRA_1].s, 0, sizeof(sBus[0].s));
	memset(sBus[BUS_EXTRA_2].s, 0, sizeof(sBus[0].s));

	reverbBefore();

	s16 bl[SUBFRAME], br[SUBFRAME];
	for (int i = 0; i < SUBFRAME; i++) {
		bl[i] = clamp16(sBus[BUS_BACK_L_REVERB].s[i]);
		br[i] = clamp16(sBus[BUS_BACK_R_REVERB].s[i]);
	}
	addWithVolume(&sBus[BUS_FRONT_L_REVERB], bl, SUBFRAME, 0, 0x7FFF);
	addWithVolume(&sBus[BUS_FRONT_R_REVERB], bl, SUBFRAME, 0, 0xB820);
	addWithVolume(&sBus[BUS_FRONT_L_REVERB], br + 0x28, 0x28, 0, 0xB820);
	addWithVolume(&sBus[BUS_FRONT_R_REVERB], br + 0x28, 0x28, 0, 0x7FFF);
	memset(sBus[BUS_BACK_L_REVERB].s, 0, sizeof(sBus[0].s));
	memset(sBus[BUS_BACK_R_REVERB].s, 0, sizeof(sBus[0].s));

	// oscillator patterns 2 and 3 change every sub-frame (noise-like sources)
	s16* p2 = &S.patterns[2 * PATTERN_SIZE];
	s32 yn2 = p2[PATTERN_SIZE - 2], yn1 = p2[PATTERN_SIZE - 1];
	for (int i = 0; i < PATTERN_SIZE; i += 2) {
		u32 t = (u32)(yn2 * yn1) - ((u32)(u16)p2[i] << 16);
		yn2   = yn1;
		yn1   = p2[i];
		p2[i] = (s16)((s32)t >> 16);
		t     = 2u * ((u32)(yn2 * yn1) + ((u32)(u16)p2[i + 1] << 16));
		yn2   = yn1;
		yn1   = p2[i + 1];
		p2[i + 1] = (s16)((s32)t >> 16);
	}
	s16* p3 = &S.patterns[3 * PATTERN_SIZE];
	yn2     = p3[PATTERN_SIZE - 2];
	yn1     = p3[PATTERN_SIZE - 1];
	s16 acc = (s16)yn1;
	s16 step = (s16)(p3[0] + (s16)((s32)((u32)(yn1 * yn2) + ((u32)yn2 << 16) + (u32)yn1) >> 16));
	step     = (s16)((step & 0x1FF) | 0x2000);
	for (int i = 0; i < PATTERN_SIZE; i++) {
		p3[i] = (s16)(acc + (i + 1) * step);
	}
}

static void finishSubframe()
{
	if (S.outLeft > 0 && S.outL && S.outR) {
		for (int i = 0; i < SUBFRAME; i++) {
			S.outL[i] = clamp16(((s32)clamp16(sBus[BUS_FRONT_L].s[i]) * S.outputVolume) >> 12);
			S.outR[i] = clamp16(((s32)clamp16(sBus[BUS_FRONT_R].s[i]) * S.outputVolume) >> 12);
		}
		S.outL += SUBFRAME;
		S.outR += SUBFRAME;
		S.outLeft--;
	}
	reverbAfter();
}

} // namespace

// ------------------------------------------------------------------ interface (audio_null.cpp)

// Command 0x81: voice blocks, resampling and oscillator tables, AFC coefficients, reverb lines.
extern "C" void ps3DspSetup(u32 voiceCount, u32 voices, u32 tables, u32 afcCoeffs, u32 reverb)
{
	S.voiceCount = voiceCount;
	S.voices     = (DSPchannel_*)(uptr)voices;
	S.reverb     = (FXBuffer*)(uptr)reverb;
	memcpy(S.resampleCoeffs, (const void*)(uptr)tables, sizeof(S.resampleCoeffs));
	memcpy(S.patterns, (const u8*)(uptr)tables + 0x200, sizeof(S.patterns));
	memcpy(S.afcCoeffs, (const void*)(uptr)afcCoeffs, sizeof(S.afcCoeffs));
	memset(S.reverbPos, 0, sizeof(S.reverbPos));
	memset(S.reverbLast8, 0, sizeof(S.reverbLast8));
	for (int i = 0; i < BUS_COUNT; i++) {
		memset(sBus[i].s, 0, sizeof(sBus[i].s));
	}
	S.configured = voices != 0 && tables != 0 && afcCoeffs != 0;
	ps3Log("[ps3] audio: DSP set up (%u voices)\n", (unsigned)voiceCount);
}

// Command 0x82: a frame of `subframes` sub-frames goes to the two output buffers.
extern "C" void ps3DspStartFrame(u32 subframes, u32 outputVolume, u32 left, u32 right)
{
	S.outputVolume = (u16)outputVolume;
	S.outL         = (s16*)(uptr)left;
	S.outR         = (s16*)(uptr)right;
	S.outLeft      = subframes;
}

// Mail 0 (release halt): mix one sub-frame.
extern "C" void ps3DspRenderSubframe(void)
{
	if (!S.configured) {
		return;
	}
	const unsigned long long t0 = ps3b_timebase();
	S.subframes++;
	S.playing = 0;
	prepareSubframe();
	for (u32 i = 0; i < S.voiceCount; i++) {
		DSPchannel_* block = &S.voices[i];
		if (i < TRACKED_VOICES) {
			// A voice that played in the last sub-frame and no longer does: say how it ended.
			const bool active = block->enabled && !block->done;
			if (S.active[i] && !active && S.loggedStops < 24) {
				S.loggedStops++;
				const char* how = S.endedHere[i] ? (S.fadeRequested[i] ? "faded out" : "sound ended")
				                                 : (S.fadeRequested[i] ? "stopped by the game during its fade-out"
				                                                       : "stopped by the game");
				ps3Log("[sound] voice %d: %s, after %u ms (loudest sample %u of 32767)\n", (int)i, how,
				       (unsigned)((S.subframes - S.startedAt[i]) * 5 / 2), (unsigned)S.peakIn[i]);
			}
			S.active[i] = active;
		}
		renderVoice(block);
	}
	S.voicesMixed += S.playing;
	finishSubframe();
	S.mixTicks += ps3b_timebase() - t0;
}

// ARAM writes over the sound data of voices that are playing (ARQPostRequest copies at once). Test build 41:
// the music sounded wrong for a few seconds while the third level loaded; test build 42's log showed the
// cause: a sound bank loaded (here, the file select's) over the notes of music still fading out, which the
// GameCube's slower disc would have let finish first. The loader now waits for those voices (in
// vi_pad_ar_card.cpp, as a slower transfer would), up to a limit; voices still reading there when the data
// is replaced are stopped instead of playing it.

// Where a voice's sound data is in ARAM. False: not from ARAM, or not playing.
static bool voiceAramRange(const DSPchannel_* v, u32* lo, u32* hi)
{
	if (!v->enabled || v->done) {
		return false;
	}
	u32 bytes;
	switch (v->samplesSourceType) {
	case SRC_AFC_LQ_ARAM:
	case SRC_AFC_HQ_ARAM: bytes = ((v->loopStartPosition + 15) >> 4) * v->samplesSourceType; break;
	case SRC_PCM8_ARAM: bytes = v->loopStartPosition; break;
	case SRC_PCM16_ARAM: bytes = v->loopStartPosition * 2; break;
	default: return false;
	}
	*lo = v->baseAddress;
	*hi = v->baseAddress + bytes;
	return true;
}

// Voices playing sound data in ARAM [dest, dest + length).
extern "C" int ps3DspVoicesReadingAram(u32 dest, u32 length)
{
	if (!S.configured || !length || !S.voices) {
		return 0;
	}
	int n = 0;
	for (u32 i = 0; i < S.voiceCount; i++) {
		u32 lo, hi;
		if (voiceAramRange(&S.voices[i], &lo, &hi) && lo < dest + length && dest < hi) {
			n++;
		}
	}
	return n;
}

// ARAM [dest, dest + length) is about to be written: voices still playing sound data there are logged, and
// if `stop` (a sound bank load) stopped at the mixer's next sub-frame.
extern "C" void ps3DspNoteAramWrite(u32 dest, u32 length, int stop)
{
	if (!S.configured || !length || !S.voices) {
		return;
	}
	for (u32 i = 0; i < S.voiceCount && i < TRACKED_VOICES; i++) {
		const DSPchannel_* v = &S.voices[i];
		u32 lo, hi;
		if (!voiceAramRange(v, &lo, &hi) || !(lo < dest + length && dest < hi)) {
			continue;
		}
		S.aramOverwrites++;
		if (stop) {
			S.stopRequested[i] = 1;
		}
		if (S.loggedAramOverwrites < 16) {
			S.loggedAramOverwrites++;
			ps3Log("[sound] ARAM 0x%06x-0x%06x written while voice %u plays from 0x%06x-0x%06x (source %u, %s, volume %d/%d, "
			       "pitch 0x%04x)%s\n",
			       (unsigned)dest, (unsigned)(dest + length), (unsigned)i, (unsigned)lo, (unsigned)hi, (unsigned)v->samplesSourceType,
			       v->isLooping ? "looping" : "not looping", (int)(s16)v->mixChannels[0].currentVolume,
			       (int)(s16)v->mixChannels[1].currentVolume, (unsigned)v->resamplingRatio, stop ? ": stopped" : "");
		}
	}
}

extern "C" void ps3DspStatus(char* buf, unsigned int size)
{
	// the mixing time since the last report: per sub-frame, and as a share of the time; per voice, in parts
	static unsigned long long sLastTicks, sLastTb, sLastSource, sLastFilter, sLastMix;
	static u32 sLastSubframes, sLastVoices;
	const unsigned long long tb = ps3b_timebase(), freq = ps3b_timebase_freq();
	const unsigned long long ticks = S.mixTicks - sLastTicks, wall = sLastTb ? tb - sLastTb : 0;
	const u32 subframes = S.subframes - sLastSubframes, voices = S.voicesMixed - sLastVoices;
	const unsigned long long source = S.sourceTicks - sLastSource, filter = S.filterTicks - sLastFilter, mix = S.mixVoiceTicks - sLastMix;
	sLastTicks     = S.mixTicks;
	sLastTb        = tb;
	sLastSubframes = S.subframes;
	sLastVoices    = S.voicesMixed;
	sLastSource    = S.sourceTicks;
	sLastFilter    = S.filterTicks;
	sLastMix       = S.mixVoiceTicks;
	const u32 usPer    = subframes && freq ? (u32)(ticks * 1000000ULL / freq / subframes) : 0;
	const u32 permille = wall ? (u32)(ticks * 1000ULL / wall) : 0;
	const u32 voicesX10 = subframes ? voices * 10 / subframes : 0;
	const unsigned long long d = voices && freq ? (unsigned long long)voices * freq / 10000000ULL : 0; // (ticks per 0.1 us, all voices)
	snprintf(buf, size,
	         "%u voices playing, %u sounds finished%s, %u playing when their ARAM data was replaced, mixing %u us per sub-frame (%u.%u%% of the "
	         "time; %u.%u voices, each: samples %u.%u us, filters %u.%u, mixing %u.%u)",
	         (unsigned)S.playing, (unsigned)S.finished, S.badVoices ? " (some with bad data)" : "", (unsigned)S.aramOverwrites, (unsigned)usPer,
	         (unsigned)(permille / 10), (unsigned)(permille % 10), (unsigned)(voicesX10 / 10), (unsigned)(voicesX10 % 10),
	         (unsigned)(d ? source / d / 10 : 0), (unsigned)(d ? source / d % 10 : 0), (unsigned)(d ? filter / d / 10 : 0),
	         (unsigned)(d ? filter / d % 10 : 0), (unsigned)(d ? mix / d / 10 : 0), (unsigned)(d ? mix / d % 10 : 0));
}
