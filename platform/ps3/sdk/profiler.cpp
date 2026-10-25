// PS3 port: a sampling profiler for the game's threads. 250 times a second it looks at the thread that
// holds the baton (os_sched.cpp): the last game code it ran (self time) and the return addresses on its
// stack (inclusive time; test build 45: walked from the stack pointer the hooks note, ps3WalkStack, instead
// of a shadow call stack kept by the hooks). While no game thread runs, it notes the last game code of the
// one waiting ("wait": e.g. GXDrawDone or the retrace). At every game section change, every 60 seconds and on exit, it logs the busiest code
// addresses and functions as "[prof]" lines; tools/ps3/profile_log.py turns them into tables with
// function names (it needs the ELF of the build that wrote the log).
//
// Time in code that is not instrumented (the SDK replacement, Nintendo's GX library, the C library)
// counts towards the game function that called it.
//
// It also samples the graphics thread while it interprets commands (ps3GfxPhase not idle): its code is
// instrumented too (include/ps3/gfx_hooks.h), and its busiest code is logged as "[prof] gfx" lines.

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#define PS3_GFX_HOOKS_NO_DEFINITIONS
#include "ps3/gfx_hooks.h"
#include <stdio.h>
#include <string.h>

extern "C" {
struct Ps3GfxHookState ps3GfxHook;
extern volatile unsigned int ps3GfxPhase; // (gpu_cp.h: 0 is idle)
extern unsigned long long ps3GfxStackLo, ps3GfxStackHi; // (gx_fifo.cpp)
}

namespace {

enum {
	SELF_SLOTS   = 1 << 14,
	INCL_SLOTS   = 1 << 14,
	MAX_THREADS  = 32,
	TOP_SELF     = 50,
	TOP_INCL     = 160, // (return addresses: several per function)
	TOP_WAIT     = 12,
	GFX_SELF_SLOTS = 1 << 12,
	GFX_INCL_SLOTS = 1 << 12,
	TOP_GFX_SELF = 60,
	TOP_GFX_INCL = 120,
	REPORT_EVERY = 250 * 60, // samples: a minute
};

template <u32 SLOTS>
struct Table {
	void* key[SLOTS];
	u32 count[SLOTS];
	u32 used;
	u32 slots;
	u32 dropped;

	void clear()
	{
		memset(key, 0, sizeof(void*) * slots);
		memset(count, 0, sizeof(u32) * slots);
		used    = 0;
		dropped = 0;
	}
	void add(void* k)
	{
		if (!k) {
			return;
		}
		u32 h = (u32)(((uptr)k >> 2) * 2654435761u) & (slots - 1);
		while (key[h] && key[h] != k) {
			h = (h + 1) & (slots - 1);
		}
		if (!key[h]) {
			if (used >= slots * 3 / 4) {
				dropped++;
				return;
			}
			key[h] = k;
			used++;
		}
		count[h]++;
	}
};

Table<SELF_SLOTS> sSelf;
Table<SELF_SLOTS> sIncl;
Table<GFX_SELF_SLOTS> sGfxSelf;
Table<GFX_INCL_SLOTS> sGfxIncl;
Table<GFX_SELF_SLOTS> sWait;
volatile u32 sMoves;     // samples where the baton holder had moved on since the previous one (watchdog)
// Since the last heartbeat (ps3ProfilerFrameStatus): samples, and those with each game thread running.
u32 sHbSamples, sHbRunning[MAX_THREADS];
void* sMovedPc;
void* sMovedSp;
int sMovedThread = -1;
u32 sGfxBusySamples;
u32 sRunningSamples[MAX_THREADS];
u32 sWaitingSamples;
u32 sNoThreadSamples;
u32 sSamples;
unsigned long long sStartedAt;
char sSection[48] = "boot";
char sNextSection[48];
volatile int sSectionChanged;
volatile int sStopped;
bool sStarted;

template <typename T>
void logTop(T& t, const char* what, int top)
{
	for (int n = 0; n < top; n++) {
		u32 best = 0, at = 0;
		for (u32 i = 0; i < t.slots; i++) {
			if (t.count[i] > best) {
				best = t.count[i];
				at   = i;
			}
		}
		if (!best) {
			break;
		}
		ps3Log("[prof] %s %u %p\n", what, (unsigned)best, t.key[at]);
		t.count[at] = 0; // (taken)
	}
}

void report(const char* why)
{
	if (!sSamples) {
		return;
	}
	unsigned long long now = ps3b_timebase();
	unsigned secs          = (unsigned)((now - sStartedAt) / ps3b_timebase_freq());
	char threads[256];
	int n      = 0;
	threads[0] = 0;
	for (int i = 0; i < MAX_THREADS; i++) {
		if (sRunningSamples[i]) {
			n += snprintf(threads + n, sizeof(threads) - n, " thread %d %u%%,", i, (unsigned)(sRunningSamples[i] * 100ULL / sSamples));
		}
	}
	ps3Log("[prof] begin \"%s\" (%s): %u samples over %u s; running:%s waiting %u%%, no thread %u%%\n", sSection, why, (unsigned)sSamples,
	       secs, threads, (unsigned)(sWaitingSamples * 100ULL / sSamples), (unsigned)(sNoThreadSamples * 100ULL / sSamples));
	logTop(sSelf, "self", TOP_SELF);
	logTop(sIncl, "incl", TOP_INCL);
	logTop(sWait, "wait", TOP_WAIT);
	ps3Log("[prof] gfx busy %u of %u samples\n", (unsigned)sGfxBusySamples, (unsigned)sSamples);
	logTop(sGfxSelf, "gfx self", TOP_GFX_SELF);
	logTop(sGfxIncl, "gfx incl", TOP_GFX_INCL);
	ps3Log("[prof] end\n");
	sSelf.clear();
	sIncl.clear();
	sGfxSelf.clear();
	sGfxIncl.clear();
	sWait.clear();
	sGfxBusySamples = 0;
	memset(sRunningSamples, 0, sizeof(sRunningSamples));
	sWaitingSamples = sNoThreadSamples = sSamples = 0;
	sStartedAt                                    = now;
}

// Inclusive: each return address on the stack once (recursion counts once).
template <typename T>
void addStack(T& t, void* const* pcs, int depth)
{
	for (int i = 0; i < depth; i++) {
		bool seen = false;
		for (int j = 0; j < i && !seen; j++) {
			seen = pcs[j] == pcs[i];
		}
		if (!seen) {
			t.add(pcs[i]);
		}
	}
}

void samplerThread(void*)
{
	Ps3SchedSample s;
	for (;;) {
		ps3b_sleep_us(4000); // (250 samples a second: each one interrupts a running thread)
		if (sStopped) {
			continue;
		}
		if (sSectionChanged) {
			report("section ends");
			snprintf(sSection, sizeof(sSection), "%s", sNextSection);
			sSectionChanged = 0;
		}
		sSamples++;
		sHbSamples++;
		if (!ps3SchedSample(&s)) {
			sNoThreadSamples++;
		} else {
			if (s.thread != sMovedThread || s.pc != sMovedPc || s.sp != sMovedSp) {
				sMoves       = sMoves + 1;
				sMovedThread = s.thread;
				sMovedPc     = s.pc;
				sMovedSp     = s.sp;
			}
			if (s.waiting) {
				sWaitingSamples++;
				sWait.add(s.pc);
			} else {
				if (s.thread >= 0 && s.thread < MAX_THREADS) {
					sRunningSamples[s.thread]++;
					sHbRunning[s.thread]++;
				}
				sSelf.add(s.pc);
				sIncl.add(s.pc); // (the function itself: the stack holds its callers)
				addStack(sIncl, s.pcs, s.depth);
			}
		}
		// The graphics thread, while it interprets commands.
		if (ps3GfxPhase != 0) {
			sGfxBusySamples++;
			sGfxSelf.add(ps3GfxHook.lastPc);
			sGfxIncl.add(ps3GfxHook.lastPc);
			void* pcs[PS3_SAMPLE_DEPTH];
			const int depth = ps3WalkStack(ps3GfxHook.lastSp, ps3GfxStackLo, ps3GfxStackHi, pcs, PS3_SAMPLE_DEPTH);
			addStack(sGfxIncl, pcs, depth);
		}
		if (sSamples >= REPORT_EVERY) {
			report("1 minute");
		}
	}
}

void onExit()
{
	sStopped = 1;
	ps3b_sleep_us(3000); // (let a sample in progress finish)
	report("exit");
}

} // namespace

extern "C" unsigned int ps3ProfilerMoves(void) { return sMoves; }

// For the heartbeat (os_sched.cpp): how much of each frame (msPerFrame long on average since the last one)
// the game threads ran, from the samples since then. The rest of the time none of them ran: waiting for the
// retrace, for the graphics thread (GXDrawDone), for the sound...
extern "C" void ps3ProfilerFrameStatus(char* buf, unsigned int size, float msPerFrame)
{
	const u32 samples = sHbSamples;
	int n             = snprintf(buf, size, "game threads, ms per frame (%.1f ms):", (double)msPerFrame);
	u32 running       = 0;
	for (int i = 0; i < MAX_THREADS && n < (int)size - 40; i++) {
		const u32 r = sHbRunning[i];
		sHbRunning[i] = 0;
		if (!r || !samples) {
			continue;
		}
		running += r;
		n += snprintf(buf + n, size - n, " thread %d %.1f,", i, (double)(msPerFrame * r / samples));
	}
	snprintf(buf + n, size - n, " none running %.1f", samples ? (double)(msPerFrame * (samples - running) / samples) : 0.0);
	sHbSamples = 0;
}

extern "C" void ps3ProfilerStart(void)
{
	if (sStarted) {
		return;
	}
	sStarted    = true;
	sSelf.slots = SELF_SLOTS;
	sIncl.slots = INCL_SLOTS;
	sGfxSelf.slots = GFX_SELF_SLOTS;
	sGfxIncl.slots = GFX_INCL_SLOTS;
	sWait.slots    = GFX_SELF_SLOTS;
	sWait.clear();
	sSelf.clear();
	sIncl.clear();
	sGfxSelf.clear();
	sGfxIncl.clear();
	sStartedAt = ps3b_timebase();
	ps3b_set_exit_hook(onExit);
	ps3b_thread_create(samplerThread, NULL, 900, 0x8000, "pikmin_profiler");
}

extern "C" void ps3ProfileSection(const char* name)
{
	if (!sStarted) {
		return;
	}
	snprintf(sNextSection, sizeof(sNextSection), "%s", name);
	__sync_synchronize();
	sSectionChanged = 1;
}
