// PS3 port: a sampling profiler for the game's threads. 1000 times a second it looks at the thread that
// holds the baton (os_sched.cpp): the last game code it ran (self time) and its shadow call stack
// (inclusive time). At every game section change, every 60 seconds and on exit, it logs the busiest code
// addresses and functions as "[prof]" lines; tools/ps3/profile_log.py turns them into tables with
// function names (it needs the ELF of the build that wrote the log).
//
// Time in code that is not instrumented (the SDK replacement, Nintendo's GX library, the C library)
// counts towards the game function that called it.

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#include <stdio.h>
#include <string.h>

namespace {

enum {
	SELF_SLOTS   = 1 << 14,
	INCL_SLOTS   = 1 << 13,
	MAX_THREADS  = 32,
	TOP_SELF     = 50,
	TOP_INCL     = 50,
	REPORT_EVERY = 60 * 1000, // samples
};

struct Table {
	void* key[SELF_SLOTS];
	u32 count[SELF_SLOTS];
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

Table sSelf;
Table sIncl;
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

void logTop(Table& t, const char* what, int top)
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
	ps3Log("[prof] end\n");
	sSelf.clear();
	sIncl.clear();
	memset(sRunningSamples, 0, sizeof(sRunningSamples));
	sWaitingSamples = sNoThreadSamples = sSamples = 0;
	sStartedAt                                    = now;
}

void samplerThread(void*)
{
	Ps3SchedSample s;
	for (;;) {
		ps3b_sleep_us(1000);
		if (sStopped) {
			continue;
		}
		if (sSectionChanged) {
			report("section ends");
			snprintf(sSection, sizeof(sSection), "%s", sNextSection);
			sSectionChanged = 0;
		}
		sSamples++;
		if (!ps3SchedSample(&s)) {
			sNoThreadSamples++;
		} else if (s.waiting) {
			sWaitingSamples++;
		} else {
			if (s.thread >= 0 && s.thread < MAX_THREADS) {
				sRunningSamples[s.thread]++;
			}
			sSelf.add(s.pc);
			// Inclusive: each function on the stack once (recursion counts once).
			for (int i = 0; i < s.depth; i++) {
				bool seen = false;
				for (int j = 0; j < i && !seen; j++) {
					seen = s.fns[j] == s.fns[i];
				}
				if (!seen) {
					sIncl.add(s.fns[i]);
				}
			}
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

extern "C" void ps3ProfilerStart(void)
{
	if (sStarted) {
		return;
	}
	sStarted    = true;
	sSelf.slots = SELF_SLOTS;
	sIncl.slots = INCL_SLOTS;
	sSelf.clear();
	sIncl.clear();
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
