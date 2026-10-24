// tools/ps3/dsp_test: checks the sound mixer (platform/ps3/sdk/dsp_mix.h) on the host.
//
// The same scripted sound session (voices of every kind starting, changing volume, fading, stopping, loops,
// filters, reverb, streamed sound, ARAM overwrites, bad data) is mixed by:
//   - the PPU mixer (dsp_mix.h with direct memory access, as dsp_hle.cpp uses it);
//   - the SPU program (platform/ps3/spu/dsp_spu.cpp built for the host, its DMA emulated and checked against
//     the MFC's rules: a transfer that would stop the SPU on the console fails here);
//   - with OLD=path/to/dsp_hle.cpp, an earlier PPU-only mixer, to show a change mixes exactly as before.
// Every run must give the same output samples, voice blocks, reverb buffers and log lines, sub-frame for
// sub-frame. Each run is a child process, so all start from the same memory.
//
//     make -C tools/ps3/dsp_test [OLD=...] && tools/ps3/dsp_test/dsp_test

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string>
#include <vector>

typedef uint8_t u8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef int64_t s64;
typedef uint64_t u64;

#include "dsp_mix.h"

using dspmix::Voice;

void dspSpuHostStart(u64 shared); // dsp_spu.cpp
void dspSpuHostServe();

#ifdef WITH_OLD
extern "C" void old_ps3DspSetup(u32 voiceCount, u32 voices, u32 tables, u32 afcCoeffs, u32 reverb);
extern "C" void old_ps3DspStartFrame(u32 subframes, u32 outputVolume, u32 left, u32 right);
extern "C" void old_ps3DspRenderSubframe(void);
extern "C" void old_ps3DspNoteAramWrite(u32 dest, u32 length, int stop);
#endif

namespace {

enum {
	ARENA_SIZE  = 48 << 20,
	ARAM_SIZE   = 16 << 20,
	VOICES      = 64,
	MRAM_SAMPLES = 0x8000,
	OUT_SUBFRAMES = 16, // room in each output buffer
};

u8* gArena;
std::string gLog;

inline u8* P(u32 address) { return (u8*)(uintptr_t)address; }
inline u32 A(const void* p) { return (u32)(uintptr_t)p; }

// FXBuffer as the PPU's compiler lays it out (an 8-byte pointer), which is also the host's.
struct HostFx {
	u16 enabled;
	u16 size;
	s16* base;
	struct {
		u16 id;
		s16 volume;
	} dest[2];
	s16 coeffs[8];
};

struct Layout {
	u32 aram, voices, tables, afc, fx, reverbBuf[4], outL, outR, mram, stops, shared;
} L;

const u16 kBusIds[] = { 0x0D00, 0x0D60, 0x0F40, 0x0CA0, 0x0E80, 0x0EE0, 0x0C00, 0x0C50, 0x0DC0, 0x0E20, 0x09A0, 0x0FA0, 0x0B00 };

struct Rng {
	u64 s;
	u32 next()
	{
		s ^= s >> 12;
		s ^= s << 25;
		s ^= s >> 27;
		return (u32)((s * 2685821657736338717ULL) >> 32);
	}
	u32 below(u32 n) { return n ? next() % n : 0; }
	bool chance(u32 perMille) { return below(1000) < perMille; }
};

void fail(const char* fmt, const char* what, u64 a, u64 b, u64 c)
{
	fprintf(stderr, fmt, what, (unsigned long long)a, (unsigned long long)b, (unsigned long long)c);
	fprintf(stderr, "\n");
	_exit(3);
}

// ---------------------------------------------------------------- memory set-up

void layOut(int misaligned, Rng& rng)
{
	u32 at = A(gArena);
	auto take = [&](u32 size, u32 align, u32 offset) {
		at       = (at + align - 1) & ~(align - 1);
		u32 here = at + offset;
		at       = here + size;
		return here;
	};
	L.aram   = take(ARAM_SIZE, 128, 0);
	L.voices = take(VOICES * 0x180, 32, misaligned ? 32 : 0);
	L.tables = take(0x400, 32, misaligned ? 2 : 0);
	L.afc    = take(0x40, 32, misaligned ? 6 : 0);
	L.fx     = take(4 * sizeof(HostFx), 32, misaligned ? 8 : 0);
	for (int r = 0; r < 4; r++) {
		L.reverbBuf[r] = take(16 * dspmix::SUBFRAME * 2, 32, misaligned ? 2 * (r + 1) : 0);
	}
	L.outL   = take(OUT_SUBFRAMES * dspmix::SUBFRAME * 2, 32, misaligned ? 2 : 0);
	L.outR   = take(OUT_SUBFRAMES * dspmix::SUBFRAME * 2, 32, misaligned ? 10 : 0);
	L.mram   = take(MRAM_SAMPLES * 2, 32, misaligned ? 2 : 0);
	L.stops  = take(PS3_DSP_TRACKED_VOICES, 16, 0);
	L.shared = take(sizeof(Ps3DspShared), 128, 0);
	if (at > A(gArena) + ARENA_SIZE) {
		fprintf(stderr, "arena too small\n");
		exit(2);
	}

	// sound data: random bytes (any byte is a valid AFC header or PCM sample)
	for (u32 i = 0; i < ARAM_SIZE; i += 4) {
		const u32 x = rng.next();
		memcpy(P(L.aram + i), &x, 4);
	}
	for (u32 i = 0; i < MRAM_SAMPLES; i++) {
		((s16*)P(L.mram))[i] = (s16)rng.next();
	}
	s16* t = (s16*)P(L.tables);
	for (int i = 0; i < 0x100; i++) {
		t[i] = (s16)rng.next(); // resampling coefficients: full range (the 64-bit sums get used)
	}
	for (int i = 0; i < 0x100; i++) {
		t[0x100 + i] = (s16)rng.next(); // oscillator patterns
	}
	s16* afc = (s16*)P(L.afc);
	for (int i = 0; i < 0x20; i++) {
		afc[i] = (s16)(rng.below(0x1000) - 0x800 + (i & 1 ? -0x400 : 0x800));
	}
	for (int r = 0; r < 4; r++) {
		s16* b = (s16*)P(L.reverbBuf[r]);
		for (int i = 0; i < 16 * dspmix::SUBFRAME; i++) {
			b[i] = (s16)rng.next();
		}
	}
	memset(P(L.voices), 0, VOICES * 0x180);
	memset(P(L.stops), 0, PS3_DSP_TRACKED_VOICES);
	memset(P(L.shared), 0, sizeof(Ps3DspShared));
}

void randomFx(Rng& rng)
{
	HostFx* fx = (HostFx*)P(L.fx);
	for (int r = 0; r < 4; r++) {
		HostFx& f  = fx[r];
		f.enabled  = (u16)(rng.chance(300) ? 0 : 1 + rng.below(3));
		f.size     = (u16)(rng.chance(100) ? 0 : 1 + rng.below(16));
		f.base     = rng.chance(50) ? NULL : (s16*)P(L.reverbBuf[r]);
		for (int d = 0; d < 2; d++) {
			f.dest[d].id     = rng.chance(100) ? 0 : (rng.chance(30) ? 0x1230 : kBusIds[rng.below(13)]);
			f.dest[d].volume = (s16)rng.next();
		}
		for (int k = 0; k < 8; k++) {
			f.coeffs[k] = (s16)(rng.below(0x2000) - 0x1000);
		}
	}
}

// ---------------------------------------------------------------- what JAudio does to the voices

const u16 kSources[] = { 0, 1, 3, 4, 7, 10, 11, 12, 5, 5, 9, 9, 9, 9, 8, 8, 16, 16, 16, 33, 33, 2 };

void startVoice(Voice* v, Rng& rng)
{
	for (u32 i = 0; i < sizeof(Voice); i += 4) {
		const u32 x = rng.next();
		memcpy((u8*)v + i, &x, 4); // (fields the start leaves alone hold whatever was there)
	}
	v->enabled            = 1;
	v->done               = 0;
	v->resetVpb           = 1;
	v->endRequested       = 0;
	v->endReached         = 0;
	v->samplesSourceType  = kSources[rng.below(sizeof(kSources) / sizeof(kSources[0]))];
	const u32 kind        = rng.below(20);
	v->resamplingRatio    = (u16)(kind == 0 ? 0 : kind == 1 ? 0x4000 + rng.below(0xC000) : 0x400 + rng.below(0x2C00));
	v->currentPosFrac     = (u16)(rng.chance(100) ? rng.next() : rng.below(0x1000));
	v->useConstantSample  = rng.chance(50);
	v->useDolbyVolume     = rng.chance(30);
	v->lowPassCoeff       = (s16)(rng.chance(500) ? 0 : rng.next());
	v->filterMode         = (s16)(rng.chance(300) ? 0x20 : (rng.chance(500) ? 0 : rng.next() & ~0x20));
	if (rng.chance(300)) {
		v->biquadFilterCoeffs[0] = 0x7FFF, v->biquadFilterCoeffs[1] = 0, v->biquadFilterCoeffs[2] = 0, v->biquadFilterCoeffs[3] = 0;
	} else {
		for (int k = 0; k < 4; k++) {
			v->biquadFilterCoeffs[k] = (s16)(rng.below(0x8000) - 0x4000);
		}
	}
	for (int c = 0; c < 6; c++) {
		dspmix::MixerChannel& ch = v->mixChannels[c];
		ch.id            = rng.chance(250) ? 0 : (rng.chance(20) ? 0x1234 : kBusIds[rng.below(13)]);
		ch.targetVolume  = (u16)(rng.chance(100) ? rng.next() : rng.below(0x8000));
		ch.currentVolume = (u16)(rng.chance(300) ? 0 : rng.chance(100) ? rng.next() : rng.below(0x8000));
	}
	const u32 length = rng.chance(100) ? rng.below(64) : 16 + rng.below(200000);
	v->loopStartPosition = length;
	v->isLooping         = rng.chance(500);
	v->loopAddress       = length ? rng.below(length) : 0;
	v->currentPosition   = length ? rng.below(length) : 0;
	v->baseAddress       = rng.chance(20) ? ARAM_SIZE - rng.below(4000) : rng.below(ARAM_SIZE - 400000);
	if (v->samplesSourceType == 33) {
		const u32 loopEnd    = 0x400 + rng.below(MRAM_SAMPLES - 0x400);
		const u32 pos        = rng.below(loopEnd);
		v->baseAddress       = rng.chance(20) ? 0 : L.mram;
		v->loopAddress       = L.mram;
		v->currentPosition   = (pos << 16) | (rng.next() & 0xFFFF);
		v->loopStartPosition = (loopEnd << 16) | (rng.next() & 0xFFFF);
		v->remainingLength   = rng.chance(100) ? rng.below(200) : rng.below(200000);
	}
}

void noteAramWrite(u32 dest, u32 length, u8* stops)
{
	const Voice* voices = (const Voice*)P(L.voices);
	for (u32 i = 0; i < VOICES; i++) {
		const Voice* v = &voices[i];
		if (!v->enabled || v->done) {
			continue;
		}
		u32 bytes;
		switch (v->samplesSourceType) {
		case 5:
		case 9: bytes = ((v->loopStartPosition + 15) >> 4) * v->samplesSourceType; break;
		case 8: bytes = v->loopStartPosition; break;
		case 16: bytes = v->loopStartPosition * 2; break;
		default: continue;
		}
		if (v->baseAddress < dest + length && dest < v->baseAddress + bytes) {
			stops[i]++;
		}
	}
}

// ---------------------------------------------------------------- the mixers

struct Mixer {
	virtual ~Mixer() {}
	virtual void setup() = 0;
	virtual void frame(u32 subframes, u32 volume) = 0;
	virtual void subframe() = 0;
	virtual void aramWrite(u32 dest, u32 length) = 0;
};

struct HostMem {
	void read(void* dst, u32 address, u32 size) { memcpy(dst, P(address), size); }
	void write(u32 address, const void* src, u32 size) { memcpy(P(address), src, size); }
	const u8* aram(u32 address, u32 size) { return (u64)address + size > ARAM_SIZE ? NULL : P(L.aram + address); }
	u32 ticks() { return 0; }
	void log(const Ps3DspLogEntry& e)
	{
		char line[256];
		dspmix::formatLog(e, line, sizeof(line));
		gLog += line;
	}
};

Ps3DspFxLayout hostFxLayout()
{
	Ps3DspFxLayout fx;
	fx.stride   = sizeof(HostFx);
	fx.base     = offsetof(HostFx, base);
	fx.baseSize = sizeof(((HostFx*)0)->base);
	fx.dest     = offsetof(HostFx, dest);
	fx.coeffs   = offsetof(HostFx, coeffs);
	fx.pad      = 0;
	return fx;
}

struct PpuMixer : Mixer {
	dspmix::Mixer<HostMem> m;
	PpuMixer()
	{
		m.reset();
		m.setStops(L.stops);
	}
	void setup() override { m.setup(VOICES, L.voices, L.tables, L.afc, L.fx, hostFxLayout()); }
	void frame(u32 subframes, u32 volume) override { m.startFrame(subframes, volume, L.outL, L.outR); }
	void subframe() override { m.renderSubframe(); }
	void aramWrite(u32 dest, u32 length) override { noteAramWrite(dest, length, P(L.stops)); }
};

struct SpuMixer : Mixer {
	Ps3DspShared* sh;
	SpuMixer()
	{
		sh           = (Ps3DspShared*)P(L.shared);
		sh->aram     = L.aram;
		sh->aramSize = ARAM_SIZE;
		dspSpuHostStart(L.shared);
	}
	void setup() override
	{
		sh->voiceCount = VOICES;
		sh->voices     = L.voices;
		sh->tables     = L.tables;
		sh->afcCoeffs  = L.afc;
		sh->reverb     = L.fx;
		sh->fx         = hostFxLayout();
		sh->setupSeq++;
	}
	void frame(u32 subframes, u32 volume) override
	{
		sh->subframes    = subframes;
		sh->outputVolume = volume;
		sh->outL         = L.outL;
		sh->outR         = L.outR;
		sh->frameSeq++;
	}
	void subframe() override
	{
		sh->asked++;
		dspSpuHostServe();
		if (sh->spu.done != sh->asked) {
			fail("%s: the SPU program did not finish the sub-frame (%llu of %llu)%llu", "spu", sh->spu.done, sh->asked, 0);
		}
		for (; sh->logRead != sh->spu.logWrite; sh->logRead++) {
			char line[256];
			dspmix::formatLog(sh->log[sh->logRead % PS3_DSP_LOG_ENTRIES], line, sizeof(line));
			gLog += line;
		}
	}
	void aramWrite(u32 dest, u32 length) override { noteAramWrite(dest, length, sh->stopSeq); }
};

#ifdef WITH_OLD
struct OldMixer : Mixer {
	void setup() override { old_ps3DspSetup(VOICES, L.voices, L.tables, L.afc, L.fx); }
	void frame(u32 subframes, u32 volume) override { old_ps3DspStartFrame(subframes, volume, L.outL, L.outR); }
	void subframe() override { old_ps3DspRenderSubframe(); }
	void aramWrite(u32 dest, u32 length) override { old_ps3DspNoteAramWrite(dest, length, 1); }
};
#endif

// ---------------------------------------------------------------- a session

u64 fnv(const void* p, u32 n, u64 h = 1469598103934665603ULL)
{
	const u8* b = (const u8*)p;
	for (u32 i = 0; i < n; i++) {
		h = (h ^ b[i]) * 1099511628211ULL;
	}
	return h;
}

// What a run records after each sub-frame.
struct Step {
	u64 voices, reverb, out, logLength;
};

void session(Mixer& mixer, u64 seed, u32 frames, std::vector<Step>& steps)
{
	Rng rng = { seed * 0x9E3779B97F4A7C15ULL + 1 };
	randomFx(rng);
	mixer.setup();
	Voice* voices = (Voice*)P(L.voices);
	for (u32 f = 0; f < frames; f++) {
		const u32 subframes = 5 + rng.below(4);
		memset(P(L.outL), 0x55, OUT_SUBFRAMES * dspmix::SUBFRAME * 2);
		memset(P(L.outR), 0x55, OUT_SUBFRAMES * dspmix::SUBFRAME * 2);
		mixer.frame(subframes, rng.chance(100) ? rng.next() & 0xFFFF : 0x1000 + rng.below(0x4000));
		for (u32 k = 0; k < subframes + (rng.chance(50) ? 2 : 0); k++) { // (sometimes more than the buffers hold)
			// JAudio, between sub-frames
			for (u32 i = 0; i < VOICES; i++) {
				Voice* v = &voices[i];
				if (v->enabled && v->done) {
					if (rng.chance(700)) {
						v->enabled = 0;
					}
				} else if (!v->enabled) {
					if (rng.chance(25)) {
						startVoice(v, rng);
					}
				} else {
					if (rng.chance(8)) {
						v->endRequested = 1;
					}
					if (rng.chance(3)) {
						v->enabled = 0;
					}
					if (rng.chance(60)) {
						v->mixChannels[rng.below(6)].targetVolume = (u16)rng.below(0x8000);
					}
					if (rng.chance(5)) {
						v->useConstantSample ^= 1;
					}
					if (rng.chance(10)) {
						v->resamplingRatio = (u16)(0x400 + rng.below(0x2C00));
					}
				}
			}
			if (rng.chance(15)) {
				const Voice* v = &voices[rng.below(VOICES)];
				mixer.aramWrite(v->baseAddress + rng.below(0x800), 1 + rng.below(0x10000));
			}
			if (rng.chance(5)) {
				randomFx(rng);
			}
			mixer.subframe();
			Step s;
			s.voices = fnv(P(L.voices), VOICES * 0x180);
			s.reverb = 0;
			for (int r = 0; r < 4; r++) {
				s.reverb = fnv(P(L.reverbBuf[r]), 16 * dspmix::SUBFRAME * 2, s.reverb ^ r);
			}
			s.out       = fnv(P(L.outR), OUT_SUBFRAMES * dspmix::SUBFRAME * 2, fnv(P(L.outL), OUT_SUBFRAMES * dspmix::SUBFRAME * 2));
			s.logLength = gLog.size();
			steps.push_back(s);
		}
	}
}

struct Result {
	std::vector<Step> steps;
	std::string log;
	bool ok;
};

// Runs a session in a child process (fresh mixer state, the same starting memory).
Result runChild(int which, u64 seed, u32 frames, int misaligned)
{
	int fds[2];
	if (pipe(fds) != 0) {
		perror("pipe");
		exit(2);
	}
	const pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		Rng rng = { seed + 12345 };
		layOut(misaligned, rng);
		Mixer* m = NULL;
		switch (which) {
		case 0: m = new PpuMixer; break;
		case 1: m = new SpuMixer; break;
#ifdef WITH_OLD
		case 2: m = new OldMixer; break;
#endif
		}
		std::vector<Step> steps;
		session(*m, seed, frames, steps);
		const u32 n = (u32)steps.size(), logBytes = (u32)gLog.size();
		FILE* out = fdopen(fds[1], "wb");
		fwrite(&n, 4, 1, out);
		fwrite(steps.data(), sizeof(Step), n, out);
		fwrite(&logBytes, 4, 1, out);
		fwrite(gLog.data(), 1, logBytes, out);
		fclose(out);
		_exit(0);
	}
	close(fds[1]);
	Result r;
	r.ok      = false;
	FILE* in  = fdopen(fds[0], "rb");
	u32 n     = 0, logBytes = 0;
	if (fread(&n, 4, 1, in) == 1) {
		r.steps.resize(n);
		if (fread(r.steps.data(), sizeof(Step), n, in) == n && fread(&logBytes, 4, 1, in) == 1) {
			r.log.resize(logBytes);
			r.ok = fread(&r.log[0], 1, logBytes, in) == logBytes;
		}
	}
	fclose(in);
	int status = 0;
	waitpid(pid, &status, 0);
	r.ok = r.ok && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	return r;
}

const char* const kNames[] = { "PPU mixer", "SPU program", "old mixer" };

bool same(const Result& a, const Result& b, int ia, int ib)
{
	if (!a.ok || !b.ok) {
		printf("  %s: run failed\n", !a.ok ? kNames[ia] : kNames[ib]);
		return false;
	}
	const size_t n = a.steps.size() < b.steps.size() ? a.steps.size() : b.steps.size();
	for (size_t i = 0; i < n; i++) {
		const Step& x = a.steps[i];
		const Step& y = b.steps[i];
		if (x.out != y.out || x.voices != y.voices || x.reverb != y.reverb || x.logLength != y.logLength) {
			printf("  %s and %s differ at sub-frame %zu:%s%s%s%s\n", kNames[ia], kNames[ib], i, x.out != y.out ? " output" : "",
			       x.voices != y.voices ? " voice blocks" : "", x.reverb != y.reverb ? " reverb buffers" : "",
			       x.logLength != y.logLength ? " log" : "");
			if (x.logLength != y.logLength) {
				size_t k = 0;
				while (k < a.log.size() && k < b.log.size() && a.log[k] == b.log[k]) {
					k++;
				}
				const size_t from = a.log.rfind('\n', k) == std::string::npos ? 0 : a.log.rfind('\n', k) + 1;
				printf("    %s: %.200s\n    %s: %.200s\n", kNames[ia], a.log.c_str() + (from < a.log.size() ? from : a.log.size()), kNames[ib],
				       b.log.c_str() + (from < b.log.size() ? from : b.log.size()));
			}
			return false;
		}
	}
	if (a.steps.size() != b.steps.size() || a.log != b.log) {
		printf("  %s and %s differ in length or log\n", kNames[ia], kNames[ib]);
		return false;
	}
	return true;
}

} // namespace

// ---------------------------------------------------------------- DMA checks (spu_mfcio.h)

void hostDmaCheck(const volatile void* ls, uint64_t ea, uint32_t size, const char* what)
{
	const uintptr_t lsa = (uintptr_t)ls;
	if (size == 0 || size > 16384) {
		fail("DMA %s: size %llu (ls 0x%llx, ea 0x%llx)", what, size, lsa, ea);
	}
	if (size < 16) {
		if ((size != 1 && size != 2 && size != 4 && size != 8) || (ea & (size - 1)) || ((lsa ^ ea) & 15)) {
			fail("DMA %s: %llu bytes, ls 0x%llx, ea 0x%llx: not naturally aligned at the same offset", what, size, lsa, ea);
		}
	} else if ((size & 15) || (ea & 15) || (lsa & 15)) {
		fail("DMA %s: %llu bytes, ls 0x%llx, ea 0x%llx: not 16-byte aligned", what, size, lsa, ea);
	}
	if (ea < A(gArena) || ea + size > (u64)A(gArena) + ARENA_SIZE) {
		fail("DMA %s: %llu bytes at 0x%llx, outside the test's memory (ls 0x%llx)", what, size, ea, lsa);
	}
}

uint32_t hostDecrementer(void) { return 0; }

#ifdef WITH_OLD
extern "C" void ps3Log(const char* fmt, ...)
{
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (strncmp(line, "[sound] ARAM ", 13) != 0 && strncmp(line, "[ps3] audio: DSP set up", 23) != 0) {
		gLog += line; // (the lines the mixer itself logs)
	}
}

extern "C" void ps3LogOnce(int* flag, const char* fmt, ...)
{
	if (*flag) {
		return;
	}
	*flag = 1;
	char line[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	gLog += line;
}

extern "C" u8* ps3AramRange(u32 address, u32 size) { return (u64)address + size > ARAM_SIZE ? NULL : P(L.aram + address); }
#endif

int main(int argc, char** argv)
{
	const u32 seeds  = argc > 1 ? (u32)atoi(argv[1]) : 6;
	const u32 frames = argc > 2 ? (u32)atoi(argv[2]) : 400;
	gArena           = (u8*)mmap(NULL, ARENA_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	if (gArena == MAP_FAILED) {
		perror("mmap");
		return 2;
	}
#ifdef WITH_OLD
	const int runs = 3;
#else
	const int runs = 2;
#endif
	int failed = 0;
	for (u32 seed = 1; seed <= seeds; seed++) {
		for (int misaligned = 0; misaligned < 2; misaligned++) {
			Result r[3];
			for (int w = 0; w < runs; w++) {
				r[w] = runChild(w, seed, frames, misaligned);
			}
			bool ok = true;
			for (int w = 1; w < runs; w++) {
				ok = same(r[0], r[w], 0, w) && ok;
			}
			size_t lines = 0;
			for (char c : r[0].log) {
				lines += c == '\n';
			}
			printf("seed %u, %s buffers: %zu sub-frames, %zu log lines: %s\n", seed, misaligned ? "unaligned" : "aligned", r[0].steps.size(), lines,
			       ok ? "same" : "DIFFERENT");
			failed += !ok;
		}
	}
	printf(failed ? "%d sessions differ\n" : "all sessions mix the same\n", failed);
	return failed ? 1 : 0;
}
