// PS3 port: the audio DSP's mixing microcode, the PPU side.
//
// The mixing itself is in dsp_mix.h. Since test build 44 it runs on an SPU (platform/ps3/spu/dsp_spu.cpp):
// this file keeps the commands in the block it shares with the SPU (Ps3DspShared, ps3/dsp_shared.h), prints
// the lines the SPU logs, and keeps a PPU mixer (the same code, reaching memory directly) that audio.cpp falls
// back to: when the SPU cannot be started, when PIKMIN/dsp_ppu.txt or dsp_sync.txt exists, or when the SPU
// stops answering.
//
// The PPU mixer runs on the graphics thread, the rescue thread or the sound thread (audio.cpp), never two at
// once (audio.cpp's mixing mutex).

#include "sdk_internal.h"
#include "../backend/ps3_backend.h"

#include "jaudio/dspinterface.h"
#include <stdio.h>
#include <string.h>

#include "dsp_mix.h"

extern "C" u8* ps3AramRange(u32 address, u32 size); // vi_pad_ar_card.cpp
extern "C" u32 ps3AramSize(void);

using dspmix::Voice;

// The voice block layout is fixed by the microcode: the mixer's copy of it (dspmix::Voice) must match JAudio's.
// (MSL's offsetof is not a constant expression for GCC, hence the builtin.)
#define CHECK_FIELD(field, offset)                                                                                                         \
	static_assert(__builtin_offsetof(DSPchannel_, field) == (offset) && __builtin_offsetof(Voice, field) == (offset), "voice block layout: " #field)
CHECK_FIELD(enabled, 0x00);
CHECK_FIELD(done, 0x02);
CHECK_FIELD(resamplingRatio, 0x04);
CHECK_FIELD(resetVpb, 0x08);
CHECK_FIELD(endReached, 0x0A);
CHECK_FIELD(useConstantSample, 0x0C);
CHECK_FIELD(mixChannels, 0x10);
CHECK_FIELD(dolbyVolumeCurrent, 0x54);
CHECK_FIELD(dolbyVolumeTarget, 0x56);
CHECK_FIELD(useDolbyVolume, 0x58);
CHECK_FIELD(currentPosFrac, 0x60);
CHECK_FIELD(afcRemainingDecodedSamples, 0x64);
CHECK_FIELD(constantSample, 0x66);
CHECK_FIELD(currentPosition, 0x68);
CHECK_FIELD(samplesBeforeLoop, 0x6C);
CHECK_FIELD(currentAramAddr, 0x70);
CHECK_FIELD(remainingLength, 0x74);
CHECK_FIELD(resampleBuffer, 0x78);
CHECK_FIELD(biquadHistory, 0xA8);
CHECK_FIELD(afcRemainingSamples, 0xB0);
CHECK_FIELD(lowPassHistory, 0xD0);
CHECK_FIELD(samplesSourceType, 0x100);
CHECK_FIELD(isLooping, 0x102);
CHECK_FIELD(loopYN1, 0x104);
CHECK_FIELD(loopYN2, 0x106);
CHECK_FIELD(filterMode, 0x108);
CHECK_FIELD(endRequested, 0x10A);
CHECK_FIELD(loopAddress, 0x110);
CHECK_FIELD(loopStartPosition, 0x114);
CHECK_FIELD(baseAddress, 0x118);
CHECK_FIELD(biquadFilterCoeffs, 0x148);
CHECK_FIELD(lowPassCoeff, 0x150);
static_assert(sizeof(DSPchannel_) == 0x180 && sizeof(Voice) == 0x180, "voice block size");
static_assert(sizeof(dspmix::MixerChannel) == sizeof(DSPMixerChannel), "mixer channel size");
#undef CHECK_FIELD

static_assert(sizeof(Ps3DspLogEntry) == 32 && sizeof(Ps3DspStats) == 64, "shared block layout");
static_assert(__builtin_offsetof(Ps3DspShared, stopSeq) == PS3_DSP_STOPSEQ_OFFSET, "shared block layout");
static_assert(__builtin_offsetof(Ps3DspShared, spu) == PS3_DSP_STATS_OFFSET, "shared block layout");
static_assert(__builtin_offsetof(Ps3DspShared, log) == PS3_DSP_LOG_OFFSET, "shared block layout");

// The block shared with the SPU mixer. (The PPU mixer reads its stop counts too.)
static Ps3DspShared sShared;

// ------------------------------------------------------------------ log lines

static void printLog(const Ps3DspLogEntry& e)
{
	char line[256];
	dspmix::formatLog(e, line, sizeof(line));
	ps3Log("%s", line);
}

// ------------------------------------------------------------------ the PPU mixer

namespace {

struct PpuMem {
	void read(void* dst, u32 address, u32 size) { memcpy(dst, (const void*)(uptr)address, size); }
	void write(u32 address, const void* src, u32 size) { memcpy((void*)(uptr)address, src, size); }
	const u8* aram(u32 address, u32 size) { return ps3AramRange(address, size); }
	u32 ticks() { return (u32)ps3b_timebase(); }
	void log(const Ps3DspLogEntry& e) { printLog(e); }
};

dspmix::Mixer<PpuMem> sMixer;
bool sMixerReady;
bool sOnSpu; // the SPU mixes (the status line shows its counters)

// For the ARAM checks (ps3DspVoicesReadingAram, ps3DspNoteAramWrite): the voice blocks, whichever mixer.
bool sConfigured;
u32 sVoiceCount;
const DSPchannel_* sVoices;
u32 sAramOverwrites, sLoggedAramOverwrites;

void mixerReady()
{
	if (!sMixerReady) {
		sMixerReady = true;
		sMixer.reset();
		sMixer.setStops((u32)(uptr)sShared.stopSeq);
	}
}

inline void syncWrites() { __asm__ volatile("lwsync" ::: "memory"); }

} // namespace

// ------------------------------------------------------------------ interface (audio.cpp)

// Command 0x81: voice blocks, resampling and oscillator tables, AFC coefficients, reverb lines.
extern "C" void ps3DspSetup(u32 voiceCount, u32 voices, u32 tables, u32 afcCoeffs, u32 reverb)
{
	mixerReady();
	Ps3DspFxLayout fx;
	fx.stride   = sizeof(FXBuffer);
	fx.base     = __builtin_offsetof(FXBuffer, circularBufferBase);
	fx.baseSize = sizeof(((FXBuffer*)0)->circularBufferBase);
	fx.dest     = __builtin_offsetof(FXBuffer, dest);
	fx.coeffs   = __builtin_offsetof(FXBuffer, filterCoeffs);
	fx.pad      = 0;
	sMixer.setup(voiceCount, voices, tables, afcCoeffs, reverb, fx);

	sVoiceCount = voiceCount;
	sVoices     = (const DSPchannel_*)(uptr)voices;
	sConfigured = sMixer.configured;

	sShared.voiceCount = voiceCount;
	sShared.voices     = voices;
	sShared.tables     = tables;
	sShared.afcCoeffs  = afcCoeffs;
	sShared.reverb     = reverb;
	sShared.fx         = fx;
	sShared.aram       = (u32)(uptr)ps3AramRange(0, 0);
	sShared.aramSize   = ps3AramSize();
	syncWrites();
	sShared.setupSeq = sShared.setupSeq + 1;
	ps3Log("[ps3] audio: DSP set up (%u voices)\n", (unsigned)voiceCount);
}

// Command 0x82: a frame of `subframes` sub-frames goes to the two output buffers.
extern "C" void ps3DspStartFrame(u32 subframes, u32 outputVolume, u32 left, u32 right)
{
	mixerReady();
	sMixer.startFrame(subframes, outputVolume, left, right);
	sShared.subframes    = subframes;
	sShared.outputVolume = outputVolume;
	sShared.outL         = left;
	sShared.outR         = right;
	if (!sShared.aram) {
		sShared.aram     = (u32)(uptr)ps3AramRange(0, 0); // (if the ARAM was allocated after the set-up)
		sShared.aramSize = ps3AramSize();
	}
	syncWrites();
	sShared.frameSeq = sShared.frameSeq + 1;
}

// Mail 0 (release halt): the PPU mixer mixes one sub-frame.
extern "C" void ps3DspRenderSubframe(void)
{
	mixerReady();
	sMixer.renderSubframe();
}

// ---- the SPU mixer's side (audio.cpp)

extern "C" u32 ps3DspSharedAddress(void) { return (u32)(uptr)&sShared; }

// The SPU mixes from now on (its counters go into the status line).
extern "C" void ps3DspOnSpu(int on) { sOnSpu = on != 0; }

// Sub-frames asked for: the SPU mixes up to there (audio.cpp then wakes it).
extern "C" void ps3DspSpuAsk(u32 asked)
{
	sShared.asked = asked;
	__asm__ volatile("sync" ::: "memory");
}

// Sub-frames the SPU has finished.
extern "C" u32 ps3DspSpuDone(void) { return *(volatile u32*)&sShared.spu.done; }

// Prints the lines the SPU logged (audio.cpp, after each sub-frame it finished).
extern "C" void ps3DspSpuLog(void)
{
	const u32 written = *(volatile u32*)&sShared.spu.logWrite;
	u32 read          = sShared.logRead;
	if (written - read > PS3_DSP_LOG_ENTRIES) {
		read = written - PS3_DSP_LOG_ENTRIES; // (cannot happen: the SPU waits for the PPU)
	}
	if (read == written) {
		return;
	}
	__asm__ volatile("lwsync" ::: "memory");
	for (; read != written; read++) {
		Ps3DspLogEntry e;
		memcpy(&e, &sShared.log[read % PS3_DSP_LOG_ENTRIES], sizeof(e));
		printLog(e);
	}
	syncWrites();
	sShared.logRead = read;
}

// The SPU was stopped after mixing `done` sub-frames, `ofFrame` of them in the frame that is going on: the PPU
// mixer takes over from there.
extern "C" void ps3DspTakeOverFromSpu(u32 ofFrame)
{
	mixerReady();
	sMixer.skipOutput(ofFrame);
	sMixer.setStops((u32)(uptr)sShared.stopSeq);
	sOnSpu = false;
}

// ARAM writes over the sound data of voices that are playing (ARQPostRequest copies at once). Test build 41:
// the music sounded wrong for a few seconds while the third level loaded; test build 42's and 43's logs
// showed sound data written over notes still playing (music fading out) at scene changes: JAudio's heap
// compaction moving banks down, and banks loaded from the disc, which the GameCube's slower disc would have
// let finish first. The disc loader now waits for those voices (in vi_pad_ar_card.cpp, as a slower transfer
// would), up to a limit; voices still reading there when the data is replaced are stopped instead of
// playing whatever lands there.

// Where a voice's sound data is in ARAM. False: not from ARAM, or not playing.
static bool voiceAramRange(const DSPchannel_* v, u32* lo, u32* hi)
{
	if (!v->enabled || v->done) {
		return false;
	}
	u32 bytes;
	switch (v->samplesSourceType) {
	case dspmix::SRC_AFC_LQ_ARAM:
	case dspmix::SRC_AFC_HQ_ARAM: bytes = ((v->loopStartPosition + 15) >> 4) * v->samplesSourceType; break;
	case dspmix::SRC_PCM8_ARAM: bytes = v->loopStartPosition; break;
	case dspmix::SRC_PCM16_ARAM: bytes = v->loopStartPosition * 2; break;
	default: return false;
	}
	*lo = v->baseAddress;
	*hi = v->baseAddress + bytes;
	return true;
}

// Voices playing sound data in ARAM [dest, dest + length).
extern "C" int ps3DspVoicesReadingAram(u32 dest, u32 length)
{
	if (!sConfigured || !length || !sVoices) {
		return 0;
	}
	int n = 0;
	for (u32 i = 0; i < sVoiceCount; i++) {
		u32 lo, hi;
		if (voiceAramRange(&sVoices[i], &lo, &hi) && lo < dest + length && dest < hi) {
			n++;
		}
	}
	return n;
}

// ARAM [dest, dest + length) is about to be written: voices still playing sound data there are logged, and
// if `stop`, stopped at the mixer's next sub-frame.
extern "C" void ps3DspNoteAramWrite(u32 dest, u32 length, int stop)
{
	if (!sConfigured || !length || !sVoices) {
		return;
	}
	for (u32 i = 0; i < sVoiceCount && i < PS3_DSP_TRACKED_VOICES; i++) {
		const DSPchannel_* v = &sVoices[i];
		u32 lo, hi;
		if (!voiceAramRange(v, &lo, &hi) || !(lo < dest + length && dest < hi)) {
			continue;
		}
		sAramOverwrites++;
		if (stop) {
			sShared.stopSeq[i] = (u8)(sShared.stopSeq[i] + 1);
		}
		if (sLoggedAramOverwrites < 16) {
			sLoggedAramOverwrites++;
			ps3Log("[sound] ARAM 0x%06x-0x%06x written while voice %u plays from 0x%06x-0x%06x (source %u, %s, volume %d/%d, "
			       "pitch 0x%04x)%s\n",
			       (unsigned)dest, (unsigned)(dest + length), (unsigned)i, (unsigned)lo, (unsigned)hi, (unsigned)v->samplesSourceType,
			       v->isLooping ? "looping" : "not looping", (int)(s16)v->mixChannels[0].currentVolume,
			       (int)(s16)v->mixChannels[1].currentVolume, (unsigned)v->resamplingRatio, stop ? ": stopped" : "");
		}
	}
	__asm__ volatile("sync" ::: "memory");
}

extern "C" void ps3DspStatus(char* buf, unsigned int size)
{
	// the mixing time since the last report: per sub-frame, and as a share of the time; per voice, in parts
	Ps3DspStats now;
	if (sOnSpu) {
		memcpy(&now, (const void*)&sShared.spu, sizeof(now));
	} else {
		now = sMixer.st;
	}
	static Ps3DspStats sLast;
	static unsigned long long sLastTb;
	static bool sLastOnSpu;
	if (sLastOnSpu != sOnSpu) {
		memset(&sLast, 0, sizeof(sLast)); // (the other mixer's counters)
		sLastOnSpu = sOnSpu;
	}
	const unsigned long long tb = ps3b_timebase(), freq = ps3b_timebase_freq();
	const unsigned long long ticks = now.mixTicks - sLast.mixTicks, wall = sLastTb ? tb - sLastTb : 0;
	const u32 subframes = now.subframes - sLast.subframes, voices = now.voicesMixed - sLast.voicesMixed;
	const unsigned long long source = now.sourceTicks - sLast.sourceTicks, filter = now.filterTicks - sLast.filterTicks,
	                         mix = now.mixVoiceTicks - sLast.mixVoiceTicks;
	sLast   = now;
	sLastTb = tb;
	const u32 usPer     = subframes && freq ? (u32)(ticks * 1000000ULL / freq / subframes) : 0;
	const u32 permille  = wall ? (u32)(ticks * 1000ULL / wall) : 0;
	const u32 voicesX10 = subframes ? voices * 10 / subframes : 0;
	const unsigned long long d = voices && freq ? (unsigned long long)voices * freq / 10000000ULL : 0; // (ticks per 0.1 us, all voices)
	char dropped[48] = "";
	if (sOnSpu && now.logDropped) {
		snprintf(dropped, sizeof(dropped), ", %u log lines left out", (unsigned)now.logDropped);
	}
	snprintf(buf, size,
	         "%u voices playing, %u sounds finished%s, %u playing when their ARAM data was replaced, mixing %u us per sub-frame %s (%u.%u%% of "
	         "the time; %u.%u voices, each: samples %u.%u us, filters %u.%u, mixing %u.%u)%s",
	         (unsigned)now.playing, (unsigned)now.finished, now.badVoices ? " (some with bad data)" : "", (unsigned)sAramOverwrites,
	         (unsigned)usPer, sOnSpu ? "on the SPU" : "on the PPU", (unsigned)(permille / 10), (unsigned)(permille % 10),
	         (unsigned)(voicesX10 / 10), (unsigned)(voicesX10 % 10), (unsigned)(d ? source / d / 10 : 0), (unsigned)(d ? source / d % 10 : 0),
	         (unsigned)(d ? filter / d / 10 : 0), (unsigned)(d ? filter / d % 10 : 0), (unsigned)(d ? mix / d / 10 : 0),
	         (unsigned)(d ? mix / d % 10 : 0), dropped);
}
