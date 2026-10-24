// PS3 port: the audio DSP's mixing microcode, reimplemented. Shared by the SPU mixer
// (platform/ps3/spu/dsp_spu.cpp, the default since test build 44), the PPU mixer it falls back to
// (dsp_hle.cpp) and the host test (tools/ps3/dsp_test).
//
// JAudio describes every playing sound as a 0x180-byte voice block (DSPchannel_ in jaudio/dspinterface.h; Voice
// below) in main memory. On the GameCube, the DSP goes through the voice blocks once per sub-frame (80 samples,
// 2.5 ms). For each voice it decodes samples (AFC/ADPCM or PCM from ARAM, PCM streamed from main memory, or a
// built-in oscillator), resamples them to the output rate, and mixes them into its buses with volume ramps.
// Then it writes the voice's progress back: position, remaining length, and the "done" flag when the sound has
// ended or a requested stop has faded out.
//
// JAudio frees voices and advances music and movie streams from that progress, so the game needs it even
// before there is any sound output. Without it, a stopped stream never finishes stopping, and the next
// cutscene waits for it forever (test 7).
//
// The behavior follows how the Dolphin emulator documents this microcode family (the "Zelda"/DAC microcode,
// light protocol, as used by Pikmin); Dolphin also named the voice block fields. This is a separate
// implementation. Differences: buses use 32-bit accumulators (saturated when stored), volume ramps end exactly
// at their target, and a few guards stop bad voice data from reading outside ARAM or looping forever. These
// make sure a voice that is asked to stop always finishes.
//
// Main memory is reached only through the memory policy M (on the SPU, by DMA). Addresses are the PPU's
// 32-bit addresses. M provides:
//   void read(void* dst, u32 address, u32 size);
//   void write(u32 address, const void* src, u32 size);
//   const u8* aram(u32 address, u32 size); // ARAM bytes, valid until the next call; NULL if out of range
//   u32 ticks();                           // a timebase-rate counter (differences are used)
//   void log(const Ps3DspLogEntry& e);     // see ps3DspFormatLog (dsp_hle.cpp)
//
// The includer defines u8, u16, s16, u32, s32, s64 and u64, and memcpy/memset, first.

#ifndef PS3_DSP_MIX_H
#define PS3_DSP_MIX_H

#include "ps3/dsp_shared.h"

namespace dspmix {

enum {
	TRACKED_VOICES = PS3_DSP_TRACKED_VOICES,
	SUBFRAME       = 0x50,  // samples per sub-frame
	VOICE_SIZE     = 0x180,
	VOICE_RW       = 0x100, // the DSP writes back only the first 0x100 bytes of a voice block
	VOICE_HEAD     = 0x10,  // enough to see whether a voice plays
	MAX_RAW        = 4 + 0x500 + 0x10,
	PATTERN_SIZE   = 0x40,
	LOGGED_VOICES  = 24,    // start and stop lines for the first sounds of a session
};

// Sample sources (Voice::samplesSourceType)
enum {
	SRC_SQUARE        = 0,
	SRC_SAW           = 1,
	SRC_SQUARE_25     = 3,
	SRC_PATTERN_1     = 4,
	SRC_AFC_LQ_ARAM   = 5, // 16 samples in 5 bytes
	SRC_PATTERN_0     = 7,
	SRC_PCM8_ARAM     = 8,
	SRC_AFC_HQ_ARAM   = 9, // 16 samples in 9 bytes
	SRC_PATTERN_0_VAR = 10,
	SRC_PATTERN_2     = 11,
	SRC_PATTERN_3     = 12,
	SRC_PCM16_ARAM    = 16,
	SRC_PCM16_MRAM    = 33, // streamed audio (movies, cutscenes)
};

// What the mixer logs (Ps3DspLogEntry::kind)
enum {
	LOG_UNKNOWN_BUS = 1, // a[0] = bus id
	LOG_BAD_VOICE,       // a[0] = what (BAD_*), a[1] = address, a[2] = size, a[3] = source
	LOG_UNKNOWN_SOURCE,  // a[0] = source
	LOG_STOP_ASKED,      // voice, a[0] = ms
	LOG_START,           // voice, a[0] = source, a[1] = address, a[2] = length, a[3] = looping, a[4] = loop start,
	                     // a[5] = pitch, a[6] = volumes (left << 16 | right)
	LOG_SURROUND,
	LOG_FIRST_FINISHED,
	LOG_ENDED,           // voice, a[0] = how (ENDED_*), a[1] = ms, a[2] = loudest sample
};
enum { BAD_PCM, BAD_AFC, BAD_STREAM, BAD_AFC_LOOP };
enum { ENDED_SOUND, ENDED_FADE, ENDED_BY_GAME, ENDED_BY_GAME_FADING };

struct MixerChannel {
	u16 id;            // the bus (see Bus::id)
	u16 targetVolume;  // 1.15
	u16 currentVolume;
	u16 level;
};

// The voice block (DSPchannel_), field for field.
struct Voice {
	u16 enabled;                    // _00
	u16 done;                       // _02
	u16 resamplingRatio;            // _04 (4.12)
	u16 _06;
	u16 resetVpb;                   // _08
	u16 endReached;                 // _0A
	u16 useConstantSample;          // _0C
	u16 samplesToKeepCount;         // _0E
	MixerChannel mixChannels[6];    // _10
	u8 _40[0x10];
	u16 dolbyVoicePosition;         // _50
	s16 dolbyReverbFactor;          // _52
	s16 dolbyVolumeCurrent;         // _54
	s16 dolbyVolumeTarget;          // _56
	u16 useDolbyVolume;             // _58
	u16 _5A, _5C, _5E;
	u16 currentPosFrac;             // _60
	u16 _62;
	u16 afcRemainingDecodedSamples; // _64
	s16 constantSample;             // _66
	u32 currentPosition;            // _68
	u16 samplesBeforeLoop;          // _6C
	u16 _6E;
	u32 currentAramAddr;            // _70
	u32 remainingLength;            // _74
	s16 resampleBuffer[4];          // _78
	u16 variableFirHistory[20];     // _80
	s16 biquadHistory[4];           // _A8
	u16 afcRemainingSamples[16];    // _B0
	s16 lowPassHistory[2];          // _D0
	u8 _D4[0x100 - 0xD4];
	u16 samplesSourceType;          // _100
	u16 isLooping;                  // _102
	s16 loopYN1;                    // _104
	s16 loopYN2;                    // _106
	s16 filterMode;                 // _108
	u16 endRequested;               // _10A
	u32 ageCounter;                 // _10C
	u32 loopAddress;                // _110
	u32 loopStartPosition;          // _114
	u32 baseAddress;                // _118
	u32 _11C;
	s16 variableFirCoeffs[20];      // _120
	s16 biquadFilterCoeffs[4];      // _148
	s16 lowPassCoeff;               // _150
	u8 padding[0x180 - 0x152];
} __attribute__((aligned(16)));

static_assert(sizeof(Voice) == VOICE_SIZE, "voice block size");

enum { TRUE16 = 1, FALSE16 = 0 };

struct Bus {
	s32 s[SUBFRAME] __attribute__((aligned(16)));
	u16 id; // the DSP memory address JAudio uses to name the bus (DSP_SetBusConnect)
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

// A reverb line (FXBuffer), read through Ps3DspFxLayout.
struct Fx {
	u16 enabled;
	u16 size; // in sub-frames
	u32 base; // the circular buffer
	u16 destId[2];
	s16 destVolume[2];
	s16 coeffs[8];
};

static inline s16 clamp16(s64 v)
{
	return (s16)(v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v));
}

static inline s16 clamp16(s32 v)
{
	return (s16)(v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v));
}

// (A note on speed: the PPU multiplies integers slowly, 64-bit ones more slowly still, and the SPU has no
// 64-bit multiply at all, so products of 16-bit values are made as 32-bit ones (they fit) and only their sums
// are 64-bit where they could overflow.)

template <class M>
class Mixer {
public:
	M mem;
	Ps3DspStats st;
	bool configured;

	void reset()
	{
		memset(&st, 0, sizeof(st));
		configured  = false;
		voiceCount  = 0;
		voices      = 0;
		reverb      = 0;
		outLeft     = 0;
		loggedOnce  = 0;
		loggedTypes = 0;
		loggedStarts = 0;
		loggedStops  = 0;
		memset(&fx, 0, sizeof(fx));
		memset(active, 0, sizeof(active));
		memset(fadeRequested, 0, sizeof(fadeRequested));
		memset(endedHere, 0, sizeof(endedHere));
		memset(startedAt, 0, sizeof(startedAt));
		memset(peakIn, 0, sizeof(peakIn));
		memset(stopSeen, 0, sizeof(stopSeen));
		stopSeqAddress = 0;
		static const u16 kIds[BUS_COUNT] = { 0x0D00, 0x0D60, 0x0F40, 0x0CA0, 0x0E80, 0x0EE0, 0x0C00,
		                                     0x0C50, 0x0DC0, 0x0E20, 0x09A0, 0x0FA0, 0x0B00 };
		memset(busById, 0, sizeof(busById));
		for (int i = 0; i < BUS_COUNT; i++) {
			memset(bus[i].s, 0, sizeof(bus[i].s));
			bus[i].id                         = kIds[i];
			busById[(kIds[i] >> 4) & 0xFF] = (u8)(i + 1); // bits 4-11 differ for every bus (then checked)
		}
	}

	// Where the stop counts are (Ps3DspShared::stopSeq); the counts now are taken as seen.
	void setStops(u32 address)
	{
		stopSeqAddress = address;
		if (address) {
			mem.read(stopSeen, address, TRACKED_VOICES);
		}
	}

	// Command 0x81: voice blocks, resampling and oscillator tables, AFC coefficients, reverb lines.
	void setup(u32 count, u32 voiceBlocks, u32 tables, u32 afcCoeffs, u32 reverbLines, const Ps3DspFxLayout& layout)
	{
		voiceCount = count;
		voices     = voiceBlocks;
		reverb     = reverbLines;
		fxLayout   = layout;
		if (tables) {
			mem.read(resampleCoeffs, tables, sizeof(resampleCoeffs));
			mem.read(patterns, tables + 0x200, sizeof(patterns));
		}
		if (afcCoeffs) {
			mem.read(this->afcCoeffs, afcCoeffs, sizeof(this->afcCoeffs));
		}
		memset(reverbPos, 0, sizeof(reverbPos));
		memset(reverbLast8, 0, sizeof(reverbLast8));
		for (int i = 0; i < BUS_COUNT; i++) {
			memset(bus[i].s, 0, sizeof(bus[i].s));
		}
		configured = voiceBlocks != 0 && tables != 0 && afcCoeffs != 0;
	}

	// Command 0x82: a frame of `subframes` sub-frames goes to the two output buffers.
	void startFrame(u32 subframes, u32 volume, u32 left, u32 right)
	{
		outputVolume = (u16)volume;
		outL         = left;
		outR         = right;
		outLeft      = subframes;
	}

	// (switching mixers: the other one mixed `n` sub-frames of this frame already)
	void skipOutput(u32 n)
	{
		for (; n && outLeft; n--) {
			outL += SUBFRAME * 2;
			outR += SUBFRAME * 2;
			outLeft--;
		}
	}

	// Mail 0 (release halt): mix one sub-frame.
	void renderSubframe()
	{
		if (!configured) {
			return;
		}
		const u32 t0 = mem.ticks();
		st.subframes++;
		st.playing = 0;
		u8 stopNow[TRACKED_VOICES] __attribute__((aligned(16)));
		const bool stops = stopSeqAddress != 0;
		if (stops) {
			mem.read(stopNow, stopSeqAddress, TRACKED_VOICES);
		}
		prepareSubframe();
		for (u32 i = 0; i < voiceCount; i++) {
			const u32 address = voices + i * VOICE_SIZE;
			mem.read(&v, address, VOICE_HEAD);
			const bool playing = v.enabled && !v.done;
			if (i < TRACKED_VOICES) {
				// A voice that played in the last sub-frame and no longer does: say how it ended.
				if (active[i] && !playing && loggedStops < LOGGED_VOICES) {
					loggedStops++;
					Ps3DspLogEntry e = entry(LOG_ENDED, i);
					e.a[0]           = endedHere[i] ? (fadeRequested[i] ? ENDED_FADE : ENDED_SOUND)
					                                : (fadeRequested[i] ? ENDED_BY_GAME_FADING : ENDED_BY_GAME);
					e.a[1]           = (st.subframes - startedAt[i]) * 5 / 2;
					e.a[2]           = peakIn[i];
					mem.log(e);
				}
				active[i] = playing;
				if (stops && stopNow[i] != stopSeen[i]) {
					// (its sound data in ARAM is being replaced: ps3DspNoteAramWrite)
					stopSeen[i] = stopNow[i];
					if (playing) {
						const u16 done = TRUE16;
						mem.write(address + 2, &done, 2); // (Voice::done)
						st.finished++;
					}
					continue;
				}
			}
			if (playing) {
				mem.read(&v, address, VOICE_SIZE);
				if (v.enabled && !v.done) {
					renderVoice(i);
					mem.write(address, &v, VOICE_RW);
				}
			}
		}
		st.voicesMixed += st.playing;
		finishSubframe();
		st.mixTicks += (u32)(mem.ticks() - t0);
	}

private:
	u32 voiceCount;
	u32 voices;  // address of the voice blocks
	u32 reverb;  // address of the 4 reverb lines (FXBuffer)
	Ps3DspFxLayout fxLayout;
	s16 resampleCoeffs[0x100] __attribute__((aligned(16)));
	s16 patterns[0x100] __attribute__((aligned(16)));
	s16 afcCoeffs[0x20] __attribute__((aligned(16)));

	u16 outputVolume; // 4.12
	u32 outL, outR;   // addresses
	u32 outLeft;      // sub-frames the CPU's output buffers still have room for

	u16 reverbPos[4];
	s16 reverbLast8[4][8];
	Fx fx[4];

	Bus bus[BUS_COUNT];
	u8 busById[256]; // bus index + 1, by bits 4-11 of the id

	Voice v; // the voice being mixed

	// diagnostics
	u32 loggedOnce; // (LOG_* bits)
	u32 loggedTypes;
	u32 loggedStarts;
	u32 loggedStops;
	// per voice, for the start/stop lines of the first sounds of a session
	u8 active[TRACKED_VOICES];        // playing in the previous sub-frame
	u8 fadeRequested[TRACKED_VOICES];
	u8 endedHere[TRACKED_VOICES];     // the mixer set "done" (sound ended, or a requested fade finished)
	u32 startedAt[TRACKED_VOICES];    // sub-frame count when it started
	u16 peakIn[TRACKED_VOICES];       // largest input sample (before volume) since it started: 0 = silent data
	u8 stopSeen[TRACKED_VOICES] __attribute__((aligned(16)));
	u32 stopSeqAddress;

	static Ps3DspLogEntry entry(u32 kind, u32 voice)
	{
		Ps3DspLogEntry e;
		memset(&e, 0, sizeof(e));
		e.kind  = (u16)kind;
		e.voice = (u16)voice;
		return e;
	}

	bool once(u32 kind)
	{
		const u32 bit = 1u << kind;
		if (loggedOnce & bit) {
			return false;
		}
		loggedOnce |= bit;
		return true;
	}

	Bus* busFor(u16 id)
	{
		if (id == 0) {
			return NULL;
		}
		const u8 b = busById[(id >> 4) & 0xFF];
		if (b && bus[b - 1].id == id) {
			return &bus[b - 1];
		}
		if (once(LOG_UNKNOWN_BUS)) {
			Ps3DspLogEntry e = entry(LOG_UNKNOWN_BUS, 0);
			e.a[0]           = id;
			mem.log(e);
		}
		return NULL;
	}

	void badVoice(Voice* p, u32 what, u32 address, u32 size)
	{
		if (once(LOG_BAD_VOICE)) {
			Ps3DspLogEntry e = entry(LOG_BAD_VOICE, 0);
			e.a[0]           = what;
			e.a[1]           = address;
			e.a[2]           = size;
			e.a[3]           = p->samplesSourceType;
			mem.log(e);
		}
		p->done = TRUE16;
		st.badVoices++;
	}

	// ------------------------------------------------------------------ sample sources

	static inline u16 neededRawSamples(const Voice* p)
	{
		// position fraction and ratio are 4.12 fixed point
		return (u16)((p->currentPosFrac + SUBFRAME * (u32)p->resamplingRatio) >> 12);
	}

	void resample(Voice* p, const s16* src, s16* dst)
	{
		u32 ratio = p->resamplingRatio;
		u32 pos   = p->currentPosFrac;
		if ((ratio >> 12) >= 4) {
			// more than 4:1: no interpolation
			for (int i = 0; i < SUBFRAME; i++) {
				pos += ratio;
				dst[i] = src[pos >> 12];
			}
		} else {
			const s16* coeffs = resampleCoeffs;
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
			p->resampleBuffer[k] = src[(pos >> 12) + k];
		}
		p->constantSample = dst[SUBFRAME - 1];
		p->currentPosFrac = pos & 0xFFF;
	}

	// PCM8 / PCM16 from ARAM (big-endian samples, as loaded from the disc)
	void pcmFromAram(Voice* p, s16* dst, u32 count, u32 bytesPerSample)
	{
		if (p->resetVpb) {
			p->remainingLength = p->loopStartPosition - p->currentPosition;
			p->currentAramAddr = p->baseAddress + p->currentPosition * bytesPerSample;
		}
		p->endReached = FALSE16;
		int guard     = 0;
		while (count) {
			if (p->endReached) {
				p->endReached = FALSE16;
				if (!p->isLooping || ++guard > 64) {
					memset(dst, 0, count * sizeof(s16));
					p->done = TRUE16;
					return;
				}
				p->currentPosition = p->loopAddress;
				p->remainingLength = p->loopStartPosition - p->currentPosition;
				p->currentAramAddr = p->baseAddress + p->currentPosition * bytesPerSample;
			}
			u32 n = p->remainingLength < count ? p->remainingLength : count;
			if (n) {
				const u8* q = mem.aram(p->currentAramAddr, n * bytesPerSample);
				if (!q) {
					badVoice(p, BAD_PCM, p->currentAramAddr, n * bytesPerSample);
					memset(dst, 0, count * sizeof(s16));
					return;
				}
				if (bytesPerSample == 1) {
					for (u32 i = 0; i < n; i++) {
						dst[i] = (s16)((u16)q[i] << 8);
					}
				} else {
					for (u32 i = 0; i < n; i++) {
						dst[i] = (s16)((q[2 * i] << 8) | q[2 * i + 1]);
					}
				}
			}
			p->remainingLength -= n;
			p->currentAramAddr += n * bytesPerSample;
			dst += n;
			count -= n;
			if (p->remainingLength == 0) {
				p->endReached = TRUE16;
			}
		}
	}

	// AFC (ADPCM variant) blocks of 16 samples: one header byte (scale, predictor), then 4-bit (9-byte blocks)
	// or 2-bit (5-byte blocks) deltas.
	bool decodeAfc(Voice* p, s16* dst, u32 blocks)
	{
		u32 blockBytes = p->samplesSourceType; // 9 or 5
		const u8* src  = mem.aram(p->currentAramAddr, blocks * blockBytes);
		if (!src) {
			badVoice(p, BAD_AFC, p->currentAramAddr, blocks * blockBytes);
			memset(dst, 0, blocks * 16 * sizeof(s16));
			return false;
		}
		p->currentAramAddr += blocks * blockBytes;

		s16* hist = (s16*)p->afcRemainingSamples; // [14] = yn2, [15] = yn1
		for (u32 b = 0; b < blocks; b++) {
			const u32 shift = (*src >> 4) & 0xF; // (the scale is 1 << shift)
			const u32 pred  = *src & 0xF;
			const s32 c0    = afcCoeffs[pred * 2];
			const s32 c1    = afcCoeffs[pred * 2 + 1];
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
				*dst++        = (s16)sample;
				yn2           = yn1;
				yn1           = sample;
			}
			hist[14] = (s16)yn2;
			hist[15] = (s16)yn1;
		}
		return true;
	}

	void afcFromAram(Voice* p, s16* dst, u32 count)
	{
		s16* cache = (s16*)p->afcRemainingSamples; // the last decoded block; its tail is not output yet
		if (p->resetVpb) {
			cache[14]                     = 0;
			cache[15]                     = 0;
			p->afcRemainingDecodedSamples = 0;
			p->remainingLength            = p->loopStartPosition; // total length in samples
			p->currentAramAddr            = p->baseAddress;
		}
		if (p->afcRemainingDecodedSamples > 16) {
			p->afcRemainingDecodedSamples = 16;
		}

		for (int guard = 0; guard < 64; guard++) {
			// first, samples already decoded
			u32 n         = p->afcRemainingDecodedSamples < count ? p->afcRemainingDecodedSamples : count;
			const s16* in = &cache[16 - p->afcRemainingDecodedSamples];
			for (u32 i = 0; i < n; i++) {
				*dst++ = in[i];
			}
			p->afcRemainingDecodedSamples -= n;
			count -= n;
			if (count == 0) {
				return;
			}

			if (count <= p->remainingLength) {
				u32 blocks  = (count + 15) >> 4;
				u32 decoded = blocks << 4;
				if (decoded < p->remainingLength) {
					p->afcRemainingDecodedSamples = decoded - count;
					p->remainingLength -= decoded;
				} else {
					p->afcRemainingDecodedSamples = p->remainingLength - count;
					p->remainingLength            = 0;
				}
				if (!decodeAfc(p, dst, blocks)) {
					return;
				}
				if (p->afcRemainingDecodedSamples) {
					for (int i = 0; i < 16; i++) {
						cache[i] = dst[decoded - 16 + i];
					}
					if (p->remainingLength == 0 && p->loopStartPosition) {
						// the sound ends inside this block: move its last samples to the end of the cache
						int top = (int)((p->loopStartPosition + 0xF) & 0xF);
						for (u32 i = 0; i < p->afcRemainingDecodedSamples && top - (int)i >= 0; i++) {
							cache[15 - i] = cache[top - i];
						}
					}
				}
				return;
			}

			// asked for more than is left: finish the sound, or loop
			if (p->remainingLength) {
				count -= p->remainingLength;
				if (!decodeAfc(p, dst, (p->remainingLength + 15) >> 4)) {
					return;
				}
				dst += p->remainingLength;
			}
			if (!p->isLooping) {
				p->done = TRUE16;
				memset(dst, 0, count * sizeof(s16));
				return;
			}
			// loop: loopAddress is the loop start in samples; source type = bytes per 16 samples
			p->currentAramAddr = p->baseAddress + (p->loopAddress >> 4) * p->samplesSourceType;
			cache[14]          = p->loopYN2;
			cache[15]          = p->loopYN1;
			if (!decodeAfc(p, cache, 1)) {
				return;
			}
			p->afcRemainingDecodedSamples = 16 - (p->loopAddress & 0xF);
			p->remainingLength            = p->loopStartPosition - p->afcRemainingDecodedSamples - p->loopAddress;
		}
		badVoice(p, BAD_AFC_LOOP, p->loopAddress, p->loopStartPosition);
		memset(dst, 0, count * sizeof(s16));
	}

	// PCM16 in main memory, read in a circular buffer: how the CPU streams movie and cutscene audio.
	void pcm16FromMram(Voice* p, s16* dst, u32 count)
	{
		u16 pos    = (u16)(p->currentPosition >> 16);
		u32 remain = p->remainingLength;
		if (p->baseAddress == 0) {
			badVoice(p, BAD_STREAM, 0, count * 2);
			memset(dst, 0, count * sizeof(s16));
			return;
		}
		const u32 src = p->baseAddress + (u32)pos * 2;
		if (count > remain) {
			if (remain) {
				mem.read(dst, src, remain * 2);
			}
			const s16 last = remain ? dst[remain - 1] : 0;
			for (u32 i = remain; i < count; i++) {
				dst[i] = last;
			}
			pos += (u16)remain;
			p->remainingLength = 0;
			p->done            = TRUE16;
		} else {
			p->remainingLength   = remain - count;
			u16 loopEnd          = (u16)(p->loopStartPosition >> 16);
			p->samplesBeforeLoop = (u16)(loopEnd - pos);
			if (count <= p->samplesBeforeLoop) {
				if (count) {
					mem.read(dst, src, count * 2);
				}
				pos += (u16)count;
			} else {
				u32 first = p->samplesBeforeLoop;
				if (first) {
					mem.read(dst, src, first * 2);
				}
				p->baseAddress = p->loopAddress;
				pos            = (u16)(count - first);
				if (pos) {
					mem.read(dst + first, p->loopAddress, (u32)pos * 2);
				}
			}
		}
		p->currentPosition = ((u32)pos << 16) | (p->currentPosition & 0xFFFF);
	}

	void oscillator(Voice* p, s16* out)
	{
		switch (p->samplesSourceType) {
		case SRC_SQUARE:
		case SRC_SQUARE_25:
		{
			u32 shift = p->samplesSourceType == SRC_SQUARE ? 1 : 2;
			u32 mask  = (1u << shift) - 1;
			u32 ratio = (u32)p->resamplingRatio << (shift - 1);
			u32 pos   = (u32)p->currentPosFrac << shift;
			for (int i = 0; i < SUBFRAME; i++) {
				out[i] = ((pos >> 16) & mask) ? (s16)0xC000 : (s16)0x4000;
				pos += ratio;
			}
			p->currentPosFrac = (u16)(pos >> shift);
			break;
		}
		case SRC_SAW:
		{
			u32 pos = p->currentPosFrac;
			for (int i = 0; i < SUBFRAME; i++) {
				out[i] = (s16)(u16)pos;
				pos += p->resamplingRatio >> 1;
			}
			p->currentPosFrac = (u16)pos;
			break;
		}
		default:
		{
			int pattern;
			bool variable = false;
			switch (p->samplesSourceType) {
			case SRC_PATTERN_0: pattern = 0; break;
			case SRC_PATTERN_0_VAR: pattern = 0; variable = true; break;
			case SRC_PATTERN_1: pattern = 1; break;
			case SRC_PATTERN_2: pattern = 2; break;
			default: pattern = 3; break;
			}
			const s16* q = &patterns[pattern * PATTERN_SIZE];
			u32 pos      = (u32)p->currentPosFrac << 6;
			u32 step     = (u32)p->resamplingRatio << 5;
			for (int i = 0; i < SUBFRAME; i++) {
				out[i] = q[(pos >> 16) & (PATTERN_SIZE - 1)];
				pos    = (pos + step) % (PATTERN_SIZE << 16);
				if (variable) {
					s32 mod = clamp16(bus[BUS_BACK_R].s[i]);
					pos     = ((pos << 10) + (u32)(mod * (s32)p->resamplingRatio)) >> 10;
				}
			}
			p->currentPosFrac = (u16)(pos >> 6);
			break;
		}
		}
	}

	void loadInput(Voice* p, s16* out)
	{
		if (p->useConstantSample) {
			// paused, or being silenced
			for (int i = 0; i < SUBFRAME; i++) {
				out[i] = p->constantSample;
			}
			return;
		}

		s16 raw[MAX_RAW] __attribute__((aligned(16)));
		for (int k = 0; k < 4; k++) {
			raw[k] = p->resampleBuffer[k];
		}
		u32 need = neededRawSamples(p);

		switch (p->samplesSourceType) {
		case SRC_SQUARE:
		case SRC_SAW:
		case SRC_SQUARE_25:
		case SRC_PATTERN_0:
		case SRC_PATTERN_0_VAR:
		case SRC_PATTERN_1:
		case SRC_PATTERN_2:
		case SRC_PATTERN_3:
			oscillator(p, out);
			return;
		case SRC_PCM8_ARAM:
			pcmFromAram(p, raw + 4, need, 1);
			break;
		case SRC_PCM16_ARAM:
			pcmFromAram(p, raw + 4, need, 2);
			break;
		case SRC_AFC_HQ_ARAM:
		case SRC_AFC_LQ_ARAM:
			afcFromAram(p, raw + 4, need);
			break;
		case SRC_PCM16_MRAM:
			pcm16FromMram(p, raw + 4, need);
			break;
		default:
		{
			const u32 bit = 1u << (p->samplesSourceType & 31);
			if (!(loggedTypes & bit)) {
				loggedTypes |= bit;
				Ps3DspLogEntry e = entry(LOG_UNKNOWN_SOURCE, 0);
				e.a[0]           = p->samplesSourceType;
				mem.log(e);
			}
			memset(out, 0, SUBFRAME * sizeof(s16));
			return;
		}
		}
		resample(p, raw, out);
	}

	// ------------------------------------------------------------------ filters and mixing

	static void lowPass(Voice* p, s16* buf)
	{
		s32 yn1   = p->resetVpb ? 0 : p->lowPassHistory[0];
		s32 xn1   = p->resetVpb ? 0 : p->lowPassHistory[1];
		s32 coeff = (u16)p->lowPassCoeff;
		for (int i = 0; i < SUBFRAME; i++) {
			s32 xn0 = buf[i];
			s64 t   = (((s64)xn0 - xn1) * coeff >> 7) + yn1;
			s16 yn0 = clamp16(t);
			buf[i]  = yn0;
			yn1     = yn0;
			xn1     = xn0;
		}
		p->lowPassHistory[0] = (s16)yn1;
		p->lowPassHistory[1] = (s16)xn1;
	}

	static void biquad(Voice* p, s16* buf)
	{
		s16* h       = p->biquadHistory;      // xn1, xn2, yn1, yn2
		const s16* c = p->biquadFilterCoeffs; // bn1, bn2, an1, an2
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
	static void mixRamp(Bus* b, const s16* src, s32 vol, s32 step)
	{
		s32* out = b->s;
		if (step == 0) {
			const s32 g = vol >> 16;
			for (int i = 0; i < SUBFRAME; i += 4) {
				out[i] += (g * src[i]) >> 16;
				out[i + 1] += (g * src[i + 1]) >> 16;
				out[i + 2] += (g * src[i + 2]) >> 16;
				out[i + 3] += (g * src[i + 3]) >> 16;
			}
			return;
		}
		for (int i = 0; i < SUBFRAME; i++) {
			out[i] += ((vol >> 16) * src[i]) >> 16;
			vol += step;
		}
	}

	static void addWithVolume(Bus* b, const s16* src, int count, int offset, u16 vol)
	{
		for (int i = 0; i < count; i++) {
			b->s[offset + i] += clamp16(((s32)src[i] * (s32)vol) >> 15);
		}
	}

	// The voice in `v` (voice block `index`): mixes one sub-frame of it and updates it.
	void renderVoice(u32 index)
	{
		st.playing++;
		const bool tracked = index < TRACKED_VOICES;
		if (v.resetVpb && tracked) {
			startedAt[index]     = st.subframes;
			fadeRequested[index] = 0;
			endedHere[index]     = 0;
			peakIn[index]        = 0;
		}
		if (v.endRequested && tracked && !fadeRequested[index]) {
			fadeRequested[index] = 1;
			if (loggedStops < LOGGED_VOICES) {
				Ps3DspLogEntry e = entry(LOG_STOP_ASKED, index);
				e.a[0]           = (st.subframes - startedAt[index]) * 5 / 2;
				mem.log(e);
			}
		}
		if (v.resetVpb && loggedStarts < LOGGED_VOICES) {
			// The first sounds of a session: shows whether music and effects start, and with what data.
			loggedStarts++;
			Ps3DspLogEntry e = entry(LOG_START, index);
			e.a[0]           = v.samplesSourceType;
			e.a[1]           = v.baseAddress;
			e.a[2]           = v.loopStartPosition;
			e.a[3]           = v.isLooping;
			e.a[4]           = v.loopAddress;
			e.a[5]           = v.resamplingRatio;
			e.a[6]           = ((u32)v.mixChannels[0].targetVolume << 16) | v.mixChannels[1].targetVolume;
			mem.log(e);
		}

		s16 in[SUBFRAME] __attribute__((aligned(16)));
		const u32 t0 = mem.ticks();
		loadInput(&v, in);
		const u32 t1 = mem.ticks();
		st.sourceTicks += (u32)(t1 - t0);
		if (tracked && loggedStops < LOGGED_VOICES) {
			u16 pk = peakIn[index];
			for (int i = 0; i < SUBFRAME; i++) {
				const u16 a = (u16)(in[i] < 0 ? -(s32)in[i] : in[i]);
				pk          = a > pk ? a : pk;
			}
			peakIn[index] = pk;
		}

		if (v.lowPassCoeff != 0) {
			lowPass(&v, in);
		}
		if ((v.filterMode & 0x20)
		    && (v.biquadFilterCoeffs[0] != 0x7FFF || v.biquadFilterCoeffs[1] != 0 || v.biquadFilterCoeffs[2] != 0
		        || v.biquadFilterCoeffs[3] != 0)) {
			biquad(&v, in);
		}

		const u32 t2 = mem.ticks();
		st.filterTicks += (u32)(t2 - t1);
		if (v.useDolbyVolume) {
			// Positional (Dolby) mixing is not part of the light microcode: silent, but stops correctly.
			if (once(LOG_SURROUND)) {
				mem.log(entry(LOG_SURROUND, index));
			}
			if (v.endRequested) {
				v.dolbyVolumeTarget = v.dolbyVolumeCurrent / 2;
				if (v.dolbyVolumeTarget == 0) {
					v.done = TRUE16;
				}
			}
			v.dolbyVolumeCurrent = v.dolbyVolumeTarget;
		} else {
			if (v.endRequested) {
				// fade out quickly: halve the volume every sub-frame, done at silence
				bool silent = true;
				for (int c = 0; c < 6; c++) {
					MixerChannel* ch = &v.mixChannels[c];
					ch->targetVolume = (u16)((s16)ch->currentVolume / 2);
					silent &= (ch->targetVolume == 0);
				}
				if (silent) {
					v.done = TRUE16;
				}
			}
			for (int c = 0; c < 6; c++) {
				MixerChannel* ch = &v.mixChannels[c];
				s16 delta        = (s16)((s16)ch->targetVolume - (s16)ch->currentVolume);
				s32 step         = ((s32)delta * 65536) / SUBFRAME;
				Bus* b           = ch->id ? busFor(ch->id) : NULL;
				if (b && (ch->currentVolume || step)) {
					mixRamp(b, in, (s32)(s16)ch->currentVolume * 65536, step);
				}
				// The ramp ends at the target, also for channels not connected to a bus. (Keeping the ramp's last
				// value instead can stop one step short forever because of rounding, and then a fading voice
				// never finishes.)
				ch->currentVolume = ch->targetVolume;
			}
		}
		st.mixVoiceTicks += (u32)(mem.ticks() - t2);

		if (!v.useConstantSample) {
			v.resetVpb = FALSE16;
		}
		if (v.done) {
			st.finished++;
			if (st.finished == 1) {
				mem.log(entry(LOG_FIRST_FINISHED, index));
			}
			if (tracked) {
				endedHere[index] = 1;
			}
		}
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

	static u16 get16(const u8* p)
	{
		u16 x;
		memcpy(&x, p, 2);
		return x;
	}

	// The 4 reverb lines' parameters (FXBuffer, as the PPU lays it out).
	void readReverb()
	{
		u8 raw[4 * 64] __attribute__((aligned(16)));
		const u32 stride = fxLayout.stride;
		if (stride < 4 || stride > 64) {
			memset(fx, 0, sizeof(fx));
			return;
		}
		mem.read(raw, reverb, 4 * stride);
		for (int r = 0; r < 4; r++) {
			const u8* p   = raw + r * stride;
			fx[r].enabled = get16(p);
			fx[r].size    = get16(p + 2);
			if (fxLayout.baseSize == 8) {
				u64 base;
				memcpy(&base, p + fxLayout.base, 8);
				fx[r].base = (u32)base;
			} else {
				u32 base;
				memcpy(&base, p + fxLayout.base, 4);
				fx[r].base = base;
			}
			for (int d = 0; d < 2; d++) {
				fx[r].destId[d]     = get16(p + fxLayout.dest + d * 4);
				fx[r].destVolume[d] = (s16)get16(p + fxLayout.dest + d * 4 + 2);
			}
			memcpy(fx[r].coeffs, p + fxLayout.coeffs, sizeof(fx[r].coeffs));
		}
	}

	void reverbBefore()
	{
		if (!reverb) {
			return;
		}
		static const int kReverbBus[4] = { BUS_REVERB_0, BUS_REVERB_1, BUS_FRONT_L_REVERB, BUS_FRONT_R_REVERB };
		readReverb();
		for (int r = 0; r < 4; r++) {
			Fx* f     = &fx[r];
			Bus* line = &bus[kReverbBus[r]];
			if (!f->enabled || !f->base || !f->size) {
				memset(line->s, 0, sizeof(line->s));
				continue;
			}
			if (reverbPos[r] >= f->size) {
				reverbPos[r] = 0;
			}
			s16 buf[SUBFRAME + 8] __attribute__((aligned(16)));
			memcpy(buf, reverbLast8[r], 8 * sizeof(s16));
			mem.read(buf + 8, f->base + (u32)reverbPos[r] * SUBFRAME * 2, SUBFRAME * sizeof(s16));
			memcpy(reverbLast8[r], buf + SUBFRAME, 8 * sizeof(s16));

			if (f->enabled & 1) {
				filter8(buf, f->coeffs);
			}
			for (int d = 0; d < 2; d++) {
				Bus* dest = busFor(f->destId[d]);
				if (dest) {
					addWithVolume(dest, buf, SUBFRAME, 0, (u16)f->destVolume[d]);
				}
			}
			if (f->enabled & 2) {
				filter8(buf, f->coeffs);
			}
			for (int i = 0; i < SUBFRAME; i++) {
				line->s[i] = buf[i];
			}
		}
	}

	void reverbAfter()
	{
		if (!reverb) {
			return;
		}
		static const int kReverbBus[4] = { BUS_REVERB_0, BUS_REVERB_1, BUS_FRONT_L_REVERB, BUS_FRONT_R_REVERB };
		for (int r = 0; r < 4; r++) {
			const Fx* f = &fx[r];
			if (!f->enabled || !f->base || !f->size) {
				continue;
			}
			const Bus* line = &bus[kReverbBus[r]];
			s16 out[SUBFRAME] __attribute__((aligned(16)));
			for (int i = 0; i < SUBFRAME; i++) {
				out[i] = clamp16(line->s[i]);
			}
			mem.write(f->base + (u32)reverbPos[r] * SUBFRAME * 2, out, sizeof(out));
			reverbPos[r] = (u16)((reverbPos[r] + 1) % f->size);
		}
	}

	// ------------------------------------------------------------------ sub-frame

	void prepareSubframe()
	{
		memset(bus[BUS_FRONT_L].s, 0, sizeof(bus[0].s));
		memset(bus[BUS_FRONT_R].s, 0, sizeof(bus[0].s));
		for (int i = 0; i < SUBFRAME; i++) {
			// the back buses decay instead of being cleared
			bus[BUS_BACK_L].s[i] = clamp16(((s32)clamp16(bus[BUS_BACK_L].s[i]) * 0x6784) >> 15);
			bus[BUS_BACK_R].s[i] = clamp16(((s32)clamp16(bus[BUS_BACK_R].s[i]) * 0x6784) >> 15);
		}
		// buses nothing reads back: keep them from growing
		memset(bus[BUS_EXTRA_0].s, 0, sizeof(bus[0].s));
		memset(bus[BUS_EXTRA_1].s, 0, sizeof(bus[0].s));
		memset(bus[BUS_EXTRA_2].s, 0, sizeof(bus[0].s));

		reverbBefore();

		s16 bl[SUBFRAME], br[SUBFRAME];
		for (int i = 0; i < SUBFRAME; i++) {
			bl[i] = clamp16(bus[BUS_BACK_L_REVERB].s[i]);
			br[i] = clamp16(bus[BUS_BACK_R_REVERB].s[i]);
		}
		addWithVolume(&bus[BUS_FRONT_L_REVERB], bl, SUBFRAME, 0, 0x7FFF);
		addWithVolume(&bus[BUS_FRONT_R_REVERB], bl, SUBFRAME, 0, 0xB820);
		addWithVolume(&bus[BUS_FRONT_L_REVERB], br + 0x28, 0x28, 0, 0xB820);
		addWithVolume(&bus[BUS_FRONT_R_REVERB], br + 0x28, 0x28, 0, 0x7FFF);
		memset(bus[BUS_BACK_L_REVERB].s, 0, sizeof(bus[0].s));
		memset(bus[BUS_BACK_R_REVERB].s, 0, sizeof(bus[0].s));

		// oscillator patterns 2 and 3 change every sub-frame (noise-like sources)
		s16* p2 = &patterns[2 * PATTERN_SIZE];
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
		s16* p3  = &patterns[3 * PATTERN_SIZE];
		yn2      = p3[PATTERN_SIZE - 2];
		yn1      = p3[PATTERN_SIZE - 1];
		s16 acc  = (s16)yn1;
		s16 step = (s16)(p3[0] + (s16)((s32)((u32)(yn1 * yn2) + ((u32)yn2 << 16) + (u32)yn1) >> 16));
		step     = (s16)((step & 0x1FF) | 0x2000);
		for (int i = 0; i < PATTERN_SIZE; i++) {
			p3[i] = (s16)(acc + (i + 1) * step);
		}
	}

	void finishSubframe()
	{
		if (outLeft > 0 && outL && outR) {
			s16 l[SUBFRAME] __attribute__((aligned(16)));
			s16 r[SUBFRAME] __attribute__((aligned(16)));
			for (int i = 0; i < SUBFRAME; i++) {
				l[i] = clamp16(((s32)clamp16(bus[BUS_FRONT_L].s[i]) * outputVolume) >> 12);
				r[i] = clamp16(((s32)clamp16(bus[BUS_FRONT_R].s[i]) * outputVolume) >> 12);
			}
			mem.write(outL, l, sizeof(l));
			mem.write(outR, r, sizeof(r));
			outL += SUBFRAME * 2;
			outR += SUBFRAME * 2;
			outLeft--;
		}
		reverbAfter();
	}
};

#if !defined(__SPU__) && !defined(DSP_SPU_HOST_TEST)
// A line the mixer logs, as text (the same whichever processor mixed). (The includer declares snprintf.)
static const char* const kBadWhat[] = { "PCM sound data in ARAM", "AFC sound data in ARAM", "streamed sound buffer", "AFC loop" };
static const char* const kEnded[]   = { "sound ended", "faded out", "stopped by the game", "stopped by the game during its fade-out" };

inline void formatLog(const Ps3DspLogEntry& e, char* buf, unsigned int size)
{
	const u32* a = e.a;
	switch (e.kind) {
	case LOG_UNKNOWN_BUS:
		snprintf(buf, size, "[ps3] audio: voice mixes to unknown DSP bus 0x%04x (ignored)\n", (unsigned)a[0]);
		break;
	case LOG_BAD_VOICE:
		snprintf(buf, size, "[ps3] audio: %s out of range (0x%08x + 0x%x, source %u): voice stopped\n", a[0] < 4 ? kBadWhat[a[0]] : "?",
		         (unsigned)a[1], (unsigned)a[2], (unsigned)a[3]);
		break;
	case LOG_UNKNOWN_SOURCE:
		snprintf(buf, size, "[ps3] audio: unknown sound source type %u (silent)\n", (unsigned)a[0]);
		break;
	case LOG_STOP_ASKED:
		snprintf(buf, size, "[sound] voice %d: the game asks it to stop, after %u ms\n", (int)e.voice, (unsigned)a[0]);
		break;
	case LOG_START:
		snprintf(buf, size, "[sound] voice %d starts: source %u at 0x%08x, length %u, loop %s at %u, pitch 0x%04x, volume %d/%d\n", (int)e.voice,
		         (unsigned)a[0], (unsigned)a[1], (unsigned)a[2], a[3] ? "on" : "off", (unsigned)a[4], (unsigned)a[5], (int)(s16)(a[6] >> 16),
		         (int)(s16)a[6]);
		break;
	case LOG_SURROUND:
		snprintf(buf, size, "[ps3] audio: a voice uses surround positioning (not supported, silent)\n");
		break;
	case LOG_FIRST_FINISHED:
		snprintf(buf, size, "[ps3] audio: DSP voice engine working (first sound finished)\n");
		break;
	case LOG_ENDED:
		snprintf(buf, size, "[sound] voice %d: %s, after %u ms (loudest sample %u of 32767)\n", (int)e.voice, a[0] < 4 ? kEnded[a[0]] : "?",
		         (unsigned)a[1], (unsigned)a[2]);
		break;
	default:
		snprintf(buf, size, "[ps3] audio: mixer log line of unknown kind %u\n", (unsigned)e.kind);
		break;
	}
}
#endif

} // namespace dspmix

#endif
