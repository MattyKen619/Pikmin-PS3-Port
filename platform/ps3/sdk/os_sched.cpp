// PS3 port: Dolphin OS threads, message queues, mutexes, condition variables and interrupts.
//
// The GameCube runs exactly one thread at a time, always the highest-priority ready one, and only
// switches threads when the running one blocks, yields, or an interrupt wakes a higher-priority thread.
// Pikmin relies on this (shared data without locks, OSDisableInterrupts as a critical section).
//
// Each OSThread runs on a real PS3 thread, but only the holder of a "baton" executes game code. Blocking
// calls hand the baton to the best ready thread. GameCube interrupts (60 Hz retrace, I/O completion)
// are queued by background threads and run by the baton holder at the next scheduling point, or by
// the idle loop when every game thread is waiting.
//
// Test build 48: the PS3 threads are kept and reused (workers). When a game thread ends, its PS3 thread
// waits for the next game thread to start instead of ending, so its stack stays where it is. On the
// GameCube a thread runs on a stack the game gives it (System::startLoading's ThreadStack), which outlives
// the thread: game code may still read something a thread left on its stack after it ended (a pointer to
// a local matrix, say). On the PS3 an ended thread's stack is gone, and lv2 stops a thread that reads it.
// Test 47's long session ended with the profiler stopping during a level load, then the watchdog stopping
// while it reported, and the game freezing: what reading a stack that is gone would do.

#include "sdk_internal.h"

#define PS3_HOOKS_NO_DEFINITIONS
#include "ps3/hooks.h"
#include "../backend/ps3_backend.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_THREADS     32
#define GAME_LV2_PRIO   1500    // all game threads share one lv2 priority; the baton decides who runs
#define GAME_STACK_SIZE 0x40000 // 256 KB: 64-bit code uses more stack than the GameCube build did
#define MAX_INTERRUPTS  256

struct ThreadImpl {
	OSThread* os;
	unsigned int cv;
	volatile int hasBaton;
	volatile int cancelled;
	int started;
	int exiting;
	OSThreadStartFunction func;
	void* arg;
	void* waitKey;
	const char* waitReason;
	u32 readySeq;
	int intrEnabled;
	int index;

	// Where the thread is, kept by the -finstrument-functions hooks (ps3/hooks.h): in ps3Hook while the
	// thread holds the baton, here while it does not (switchHooks).
	Ps3HookState hook;
	// Its PS3 thread's stack (ps3WalkStack), written under sLock: stackSeq is odd while it changes, so a
	// reader without the lock (the profiler, the watchdog) can tell a consistent pair from a torn one.
	volatile unsigned int stackSeq;
	volatile unsigned long long stackLo, stackHi;
	struct Worker* worker; // the PS3 thread it runs on (NULL: the main thread, or not started)
};

// A PS3 thread for game threads (test build 48): runs one game thread after another, never ends.
struct Worker {
	unsigned int cv;            // signalled when it is given a game thread (job)
	ThreadImpl* volatile job;   // the game thread it runs; NULL while it waits for one
	unsigned long long stackLo, stackHi;
	void* exitJump[5];          // __builtin_setjmp: where an ending game thread goes back to (runJob)
	int index;
};

struct Interrupt {
	Ps3InterruptFn fn;
	void* arg;
};

static unsigned int sLock;
static unsigned int sIdleCv;
static ThreadImpl sImpls[MAX_THREADS];
static int sImplCount;
static ThreadImpl* sRunning;
static Worker sWorkers[MAX_THREADS];
static int sWorkerCount;
static Worker* sIdleWorkers[MAX_THREADS];
static int sIdleWorkerCount;

// The hooks' state for the baton holder (ps3/hooks.h), and the queued-interrupt hint they check.
extern "C" {
Ps3HookState ps3Hook;
volatile unsigned int ps3HookPending;
}

// The hook state of a thread: in ps3Hook while it holds the baton.
static inline Ps3HookState& hookOf(ThreadImpl* t) { return t == sRunning ? ps3Hook : t->hook; }

// The baton passes from `from` to `to`: their hook states trade places.
static void switchHooks(ThreadImpl* from, ThreadImpl* to)
{
	if (from) {
		from->hook = ps3Hook;
	}
	ps3Hook = to->hook;
}

// Walks a stack (PowerPC 64-bit ELF ABI: each frame starts with a pointer to the caller's frame, and a
// function that calls others saves its return address 16 bytes into its caller's frame), from sp, staying
// within [lo, hi) so that a frame being changed while it is read cannot send the walk outside the stack.
// The first frame may be one that was just left (lastSp of a function that has returned): its link to the
// caller's frame is usually still there.
//
// Test build 49: every page it reads is checked first (ps3b_address_readable). lv2 ends a game thread that
// faults and frees its stack; in tests 47 and 48 the profiler, then the watchdog, then the exit report each
// walked the crashed main thread's stack and were ended in turn, so nothing reported the crash itself.
static int sCheckPages = 1;     // 0 if the check does not work here (ps3SchedInit tries it)
static volatile int sStackGone; // a walk found a game thread's stack gone (reported by the watchdog)

static bool pageReadable(unsigned long long addr, unsigned long long* checked)
{
	const unsigned long long page = addr & ~0xFFFull;
	if (!sCheckPages || page == *checked) {
		return true;
	}
	if (!ps3b_address_readable(page)) {
		sStackGone = 1;
		return false;
	}
	*checked = page;
	return true;
}

extern "C" int ps3WalkStack(const void* sp, unsigned long long lo, unsigned long long hi, void** pcs, int max)
{
	unsigned long long at      = (unsigned long long)(uptr)sp;
	unsigned long long checked = 0;
	int n                      = 0;
	if (!lo || hi <= lo || hi - lo > (16u << 20)) {
		return 0; // (no stack, or not one: lv2 stacks are at most a few MB)
	}
	while (n < max) {
		if ((at & 15) || at < lo || at + 8 > hi || !pageReadable(at, &checked)) {
			break;
		}
		const unsigned long long next = *(volatile const unsigned long long*)(uptr)at;
		if (next <= at || (next & 15) || next + 24 > hi || !pageReadable(next + 16, &checked)) {
			break;
		}
		void* ret = *(void* volatile const*)(uptr)(next + 16);
		if (!ret) {
			break;
		}
		pcs[n++] = ret;
		at       = next;
	}
	return n;
}

// (sLock held)
static void setStack(ThreadImpl* t, unsigned long long lo, unsigned long long hi)
{
	t->stackSeq = t->stackSeq + 1; // odd: changing
	__asm__ volatile("lwsync" ::: "memory");
	t->stackLo = lo;
	t->stackHi = hi;
	__asm__ volatile("lwsync" ::: "memory");
	t->stackSeq = t->stackSeq + 1;
}

// The game thread no longer runs there: nothing walks that stack for it any more.
static void forgetStack(ThreadImpl* t) { setStack(t, 0, 0); }

// Without the lock: false if the bounds were changing (or the thread has none).
static bool readStack(const ThreadImpl* t, unsigned long long* lo, unsigned long long* hi)
{
	const unsigned int s1 = t->stackSeq;
	__asm__ volatile("lwsync" ::: "memory");
	*lo = t->stackLo;
	*hi = t->stackHi;
	__asm__ volatile("lwsync" ::: "memory");
	return !(s1 & 1) && t->stackSeq == s1 && *lo != 0;
}

static u32 sSeq;
static volatile int sIntrEnabled = 1;
static volatile int sInInterrupt = 0;
static OSThread sDefaultThread;

static Interrupt sIntr[MAX_INTERRUPTS];
static volatile u32 sIntrHead, sIntrTail;
static volatile u32 sIntrDropped;

static volatile u32 sActivity;
static const char* volatile sLastActivity = "boot";

// Interrupt latency (diagnostics): how long queued interrupts wait for the baton holder to run them.
// The audio thread must run every 17.5 ms; a longer wait plays a block of sound twice. The heartbeat
// shows the longest wait since the last report and what the game was doing at the time.
static unsigned long long sIntrQueuedTb;    // when the queue last went from empty to non-empty
static unsigned long long sWorstWaitTb;     // longest wait since the last report
static const char* volatile sWorstWaitWhat; // last SDK call or wait reason when it ended
static int sWorstWaitThread = -1;
static volatile u32 sLongWaits;             // waits over 10 ms since the last report
static unsigned long long sTbPer10ms;       // timebase ticks in 10 ms

static void lockS() { ps3b_mutex_lock(sLock); }
static void unlockS() { ps3b_mutex_unlock(sLock); }

// ------------------------------------------------------------------ logging

extern "C" void ps3Log(const char* fmt, ...)
{
	char buf[1536];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ps3b_log_write(buf);
}

extern "C" void ps3LogOnce(int* flag, const char* fmt, ...)
{
	if (*flag) {
		return;
	}
	*flag = 1;
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ps3b_log_write(buf);
}

extern "C" void ps3NoteActivity(const char* what)
{
	sLastActivity = what;
	sActivity++;
}

// ------------------------------------------------------------------ core scheduling (sLock held)

static ThreadImpl* findImpl(OSThread* t)
{
	for (int i = 0; i < sImplCount; i++) {
		if (sImpls[i].os == t) {
			return &sImpls[i];
		}
	}
	return NULL;
}

static int eligible(ThreadImpl* t)
{
	return t->os && t->os->state == OS_THREAD_STATE_READY && t->os->suspend <= 0 && !t->cancelled && !t->exiting;
}

static ThreadImpl* pickReady(ThreadImpl* exclude)
{
	ThreadImpl* best = NULL;
	for (int i = 0; i < sImplCount; i++) {
		ThreadImpl* t = &sImpls[i];
		if (t == exclude || !eligible(t)) {
			continue;
		}
		if (!best || t->os->priority < best->os->priority
		    || (t->os->priority == best->os->priority && t->readySeq < best->readySeq)) {
			best = t;
		}
	}
	return best;
}

static void makeReady(ThreadImpl* t, int front)
{
	t->os->state  = OS_THREAD_STATE_READY;
	t->waitKey    = NULL;
	t->waitReason = NULL;
	t->readySeq   = front ? 0 : ++sSeq;
}

static void wakeAllLocked(void* key)
{
	for (int i = 0; i < sImplCount; i++) {
		ThreadImpl* t = &sImpls[i];
		if (t->os && t->os->state == OS_THREAD_STATE_WAITING && t->waitKey == key) {
			makeReady(t, 0);
		}
	}
}

// A game thread ends (sLock held): its PS3 thread goes back to its worker loop (runJob), from however deep
// in the game's calls it is, and waits there for the next game thread.
static void leaveThread(ThreadImpl* self) __attribute__((noreturn, noinline));
static void leaveThread(ThreadImpl* self)
{
	Worker* w = self->worker;
	if (!w) {
		unlockS(); // (not a worker's: cannot happen, the main thread does not end this way)
		ps3b_thread_exit();
	}
	__builtin_longjmp(w->exitJump, 1);
}

static void waitBaton(ThreadImpl* self)
{
	while (!self->hasBaton && !self->cancelled) {
		ps3b_cond_wait(self->cv, 100000);
	}
	if (self->cancelled && !self->hasBaton) {
		leaveThread(self);
	}
}

static void workerEntry(void* p);

// The baton holder starts `t` (sLock held): a waiting worker takes it, or a new one is made.
static void startPs3Thread(ThreadImpl* t)
{
	t->started = 1;
	if (sIdleWorkerCount > 0) {
		Worker* w = sIdleWorkers[--sIdleWorkerCount];
		w->job    = t;
		t->worker = w;
		ps3b_cond_signal(w->cv);
		return;
	}
	if (sWorkerCount >= MAX_THREADS) {
		ps3Log("[ps3] FAILED: no PS3 thread for OS thread %d (all %d in use)\n", t->index, sWorkerCount);
		return;
	}
	Worker* w = &sWorkers[sWorkerCount];
	w->index  = sWorkerCount;
	w->job    = t;
	t->worker = w;
	ps3b_cond_create(&w->cv, sLock);
	char name[24];
	snprintf(name, sizeof(name), "pikmin_os%d", w->index);
	if (ps3b_thread_create(workerEntry, w, GAME_LV2_PRIO, GAME_STACK_SIZE, name) != 0) {
		ps3Log("[ps3] FAILED to create PS3 thread for OS thread %d\n", t->index);
		t->worker = NULL;
		return;
	}
	sWorkerCount++;
	ps3Log("[ps3] PS3 thread %d made (for OS thread %d)\n", w->index, t->index);
}

// Runs queued interrupts on behalf of `self` (the baton holder or the idle loop). Returns how many ran.
static int deliverLocked(ThreadImpl* self, int force)
{
	(void)self;
	if (sInInterrupt || (!force && !sIntrEnabled)) {
		return 0;
	}
	if (sIntrHead != sIntrTail) {
		unsigned long long wait = ps3b_timebase() - sIntrQueuedTb;
		if (!sTbPer10ms) {
			sTbPer10ms = ps3b_timebase_freq() / 100;
		}
		if (wait > sTbPer10ms) {
			sLongWaits++;
		}
		if (wait > sWorstWaitTb) {
			sWorstWaitTb     = wait;
			sWorstWaitWhat   = sLastActivity;
			sWorstWaitThread = sRunning ? sRunning->index : -1;
		}
	}
	int ran = 0;
	while (sIntrHead != sIntrTail) {
		Interrupt in = sIntr[sIntrHead % MAX_INTERRUPTS];
		sIntrHead++;
		sInInterrupt = 1;
		unlockS();
		in.fn(in.arg);
		lockS();
		sInInterrupt = 0;
		ran++;
	}
	ps3HookPending = sIntrHead != sIntrTail;
	return ran;
}

// The running thread `self` changed its own state (WAITING, READY, exiting) or wants a preemption check.
// Returns when `self` holds the baton again (or immediately if it is exiting).
static void schedule(ThreadImpl* self)
{
	if (self->os->state == OS_THREAD_STATE_RUNNING) {
		makeReady(self, 1);
	}
	for (;;) {
		ThreadImpl* next = pickReady(NULL);
		if (!next) {
			// Nothing can run: be the idle loop. Run interrupts; otherwise wait for one to be posted.
			if (!deliverLocked(self, 1)) {
				ps3b_cond_wait(sIdleCv, 2000);
			}
			continue;
		}
		if (next == self) {
			self->os->state = OS_THREAD_STATE_RUNNING;
			return;
		}
		self->hasBaton    = 0;
		self->intrEnabled = sIntrEnabled;
		next->hasBaton    = 1;
		next->os->state   = OS_THREAD_STATE_RUNNING;
		switchHooks(self, next);
		sRunning          = next;
		sIntrEnabled      = next->intrEnabled;
		sActivity++;
		if (!next->started) {
			startPs3Thread(next);
		} else {
			ps3b_cond_signal(next->cv);
		}
		if (self->exiting) {
			return;
		}
		waitBaton(self);
		return;
	}
}

static void blockLocked(ThreadImpl* self, void* key, const char* reason)
{
	self->os->state   = OS_THREAD_STATE_WAITING;
	self->waitKey     = key;
	self->waitReason  = reason;
	sLastActivity     = reason;
	schedule(self);
}

static void preemptCheck(ThreadImpl* self)
{
	if (sInInterrupt || !self) {
		return;
	}
	ThreadImpl* best = pickReady(self);
	if (best && best->os->priority < self->os->priority) {
		schedule(self);
	}
}

// Runs game thread `t` on worker `w` (sLock held on entry and on return). The game thread ends by
// returning from its function or by OSExitThread, or is cancelled while it waits: leaveThread jumps back
// here in each case, with sLock held.
static __attribute__((noinline)) void runJob(Worker* w, ThreadImpl* t)
{
	ThreadImpl* volatile vt = t; // (read again after the jump back: registers are not kept)
	setStack(t, w->stackLo, w->stackHi);
	if (__builtin_setjmp(w->exitJump) == 0) {
		waitBaton(t);
		unlockS();
		ps3NoteActivity("thread start");
		void* result = t->func(t->arg);
		OSExitThread(result); // (does not return: leaveThread)
	}
	ThreadImpl* done = vt;
	forgetStack(done);
	done->worker = NULL;
}

static void workerEntry(void* p)
{
	Worker* w = (Worker*)p;
	ps3b_thread_stack(&w->stackLo, &w->stackHi);
	lockS();
	for (;;) {
		ThreadImpl* t = w->job;
		if (!t) {
			ps3b_cond_wait(w->cv, 1000 * 1000);
			continue;
		}
		runJob(w, t);
		w->job                           = NULL;
		sIdleWorkers[sIdleWorkerCount++] = w; // (waits for the next game thread)
	}
}

// ------------------------------------------------------------------ interrupts

extern "C" void ps3PostInterrupt(Ps3InterruptFn fn, void* arg)
{
	lockS();
	if (sIntrTail - sIntrHead >= MAX_INTERRUPTS) {
		sIntrDropped++;
	} else {
		if (sIntrTail == sIntrHead) {
			sIntrQueuedTb = ps3b_timebase();
		}
		sIntr[sIntrTail % MAX_INTERRUPTS].fn  = fn;
		sIntr[sIntrTail % MAX_INTERRUPTS].arg = arg;
		sIntrTail++;
		ps3HookPending = 1;
	}
	ps3b_cond_broadcast(sIdleCv);
	unlockS();
}

extern "C" void ps3DeliverInterrupts(void)
{
	if (sIntrHead == sIntrTail || sInInterrupt || !sIntrEnabled || !sRunning) {
		return;
	}
	lockS();
	ThreadImpl* self = sRunning;
	if (deliverLocked(self, 0)) {
		preemptCheck(self);
	}
	unlockS();
}

extern "C" int ps3InInterrupt(void) { return sInInterrupt; }
extern "C" int ps3CanBlock(void) { return sRunning != NULL && !sInInterrupt; }
// Can give up the baton without the game noticing: also not inside an OSDisableInterrupts section, where
// the GameCube would not switch threads.
extern "C" int ps3CanYield(void) { return sRunning != NULL && !sInInterrupt && sIntrEnabled; }

// ------------------------------------------------------------------ preemption points
//
// On the GameCube an interrupt can stop the running thread at any instruction and switch to a
// higher-priority thread (the audio thread wakes every 17.5 ms this way). Pikmin depends on it: the main
// thread busy-waits until the audio thread has set things up. Here the game code is compiled with
// -finstrument-functions, so every function entry calls the hook below. If an interrupt is pending,
// the hook delivers it and, if that woke a higher-priority thread, hands over the baton right there,
// as the GameCube would have. The hooks also keep a shadow call stack per thread for stall reports.
// Busy-wait loops that call no function at all use PS3_SPIN_WAIT() (types.h) instead.

extern "C" void __cyg_profile_func_enter(void* fn, void* site) __attribute__((no_instrument_function));
extern "C" void __cyg_profile_func_exit(void* fn, void* site) __attribute__((no_instrument_function));

// ---- Where NaNs come from (debugging aid, off by default: build with -DPS3_TRACE_NAN=1)
// The PPU's FPSCR has sticky bits for the invalid operations that create a NaN: 0/0, inf-inf, inf*0,
// inf/inf, square root of a negative number, and use of a signalling NaN. Arithmetic on an existing NaN
// does not set them, so they mark the origin. The hooks check them at every game function entry and
// exit; the first time a code location creates a NaN, it is logged with the thread's call stack
// ("last code run" is the basic block that did it, or one shortly after it).
//
// Test 11 showed that Pikmin makes NaNs in normal play, about 5000 a second in cutscenes (for example
// MapMgr::getCurrTri divides by the y of a wall's normal, which is 0), and that reading the FPSCR in
// every hook costs about a third of the frame rate. NaNs are harmless as long as they are handled as on
// the GameCube (GC_INDEX in types.h).
#if !defined(PS3_TRACE_NAN)
#define PS3_TRACE_NAN 0
#endif
// (FPSCR_VX* bit masks: Dolphin/PPCArch.h)
#define FPSCR_NAN_CREATED (FPSCR_VXSNAN | FPSCR_VXISI | FPSCR_VXIDI | FPSCR_VXZDZ | FPSCR_VXIMZ | FPSCR_VXSQRT)
enum { MAX_NAN_SITES = 24 };

static volatile u32 sNanCount;
static void* sNanSites[MAX_NAN_SITES];
static int sNanSiteCount;

static inline u32 readFpscr(void) __attribute__((always_inline, no_instrument_function));
static inline u32 readFpscr(void)
{
	union {
		double d;
		unsigned long long u;
	} v;
	__asm__ volatile("mffs %0" : "=f"(v.d));
	return (u32)v.u;
}

static void logStack(int index, ThreadImpl* t);
static void noteNanOrigin(ThreadImpl* t, u32 fpscr) __attribute__((noinline, no_instrument_function));
static void noteNanOrigin(ThreadImpl* t, u32 fpscr)
{
	// Clear the sticky bits (FX, the invalid-operation bits) so the next origin is seen too.
	__asm__ volatile("mtfsb0 0\n\tmtfsb0 7\n\tmtfsb0 8\n\tmtfsb0 9\n\tmtfsb0 10\n\tmtfsb0 11\n\tmtfsb0 22");
	sNanCount++;
	void* pc = hookOf(t).lastPc;
	for (int i = 0; i < sNanSiteCount; i++) {
		if (sNanSites[i] == pc) {
			return;
		}
	}
	if (sNanSiteCount >= MAX_NAN_SITES) {
		return;
	}
	sNanSites[sNanSiteCount++] = pc;
	ps3Log("[ps3] NaN created (%s%s%s%s%s%s) in thread %d:\n", (fpscr & FPSCR_VXZDZ) ? " 0/0" : "",
	       (fpscr & FPSCR_VXISI) ? " inf-inf" : "", (fpscr & FPSCR_VXIMZ) ? " inf*0" : "", (fpscr & FPSCR_VXIDI) ? " inf/inf" : "",
	       (fpscr & FPSCR_VXSQRT) ? " sqrt(negative)" : "", (fpscr & FPSCR_VXSNAN) ? " signalling NaN" : "", (int)(t - sImpls));
	logStack((int)(t - sImpls), t);
}

#if PS3_TRACE_NAN
#define CHECK_NAN_ORIGIN(t)                                    \
	do {                                                       \
		u32 fpscr_ = readFpscr();                              \
		if (__builtin_expect(fpscr_ & FPSCR_NAN_CREATED, 0)) { \
			noteNanOrigin((t), fpscr_);                        \
		}                                                      \
	} while (0)
#else
#define CHECK_NAN_ORIGIN(t) ((void)0)
#endif

extern "C" unsigned int ps3NanCount(void) { return sNanCount; }

// For the sampling profiler (profiler.cpp): what the baton holder is doing right now. Read without the
// lock (a sample may be slightly inconsistent; that is fine for a profile).
extern "C" int ps3SchedSample(Ps3SchedSample* out)
{
	ThreadImpl* t = sRunning;
	if (!t || !t->os) {
		return 0;
	}
	const Ps3HookState& h = hookOf(t);
	out->thread  = t->index;
	out->waiting = t->os->state != OS_THREAD_STATE_RUNNING;
	out->pc      = h.lastPc;
	out->sp      = h.lastSp;
	unsigned long long lo, hi;
	out->depth = readStack(t, &lo, &hi) ? ps3WalkStack(out->sp, lo, hi, out->pcs, PS3_SAMPLE_DEPTH) : 0;
	return 1;
}

// The hooks themselves are in ps3/hooks.h, compiled into each game source file (test build 36). These
// out-of-line copies serve game code built without that header (and keep the NaN-origin checks, which
// need PS3_TRACE_NAN and a build that leaves out the header).
extern "C" void __cyg_profile_func_enter(void* fn, void* site)
{
	(void)site;
	ThreadImpl* t = sRunning;
	if (t) {
		CHECK_NAN_ORIGIN(t); // the caller is the function that ran last
	}
	void* sp;
	__asm__("mr %0,1" : "=r"(sp));
	ps3Hook.lastSp = sp;
	ps3Hook.lastPc = fn;
}

extern "C" void __cyg_profile_func_exit(void* fn, void* site)
{
	(void)fn;
	ThreadImpl* t = sRunning;
	if (t) {
		CHECK_NAN_ORIGIN(t); // the returning function ran last
	}
	void* sp;
	__asm__("mr %0,1" : "=r"(sp));
	ps3Hook.lastSp = sp;
	ps3Hook.lastPc = site;
	if (ps3HookPending) {
		ps3HookDeliver(site);
	}
}

// From the hooks, while interrupts are queued: run them if they may run now (not while they are disabled,
// nor inside a handler).
extern "C" void ps3HookDeliver(void* pc)
{
	if (sIntrEnabled && !sInInterrupt && sRunning) {
		ps3DeliverInterrupts();
		ps3Hook.lastPc = pc; // interrupt handlers ran on this thread: point back at the interrupted code
	}
}

// Basic-block hooks (-fsanitize-coverage=trace-pc): game code was built with them up to test build 35, a
// call every few instructions; now only the function hooks run. Kept for code built with them.
extern "C" void __sanitizer_cov_trace_pc(void) __attribute__((no_instrument_function));
extern "C" void __sanitizer_cov_trace_pc(void)
{
	void* pc       = __builtin_return_address(0);
	ps3Hook.lastPc = pc;
	if (__builtin_expect(ps3HookPending != 0, 0)) {
		ps3HookDeliver(pc);
	}
}

extern "C" void ps3SpinWait(void)
{
	if (sIntrHead != sIntrTail) {
		ps3DeliverInterrupts();
	} else {
		ps3b_sleep_us(20);
	}
}

extern "C" void ps3PreemptPoint(void)
{
	if (sIntrHead != sIntrTail && sIntrEnabled && !sInInterrupt) {
		ps3DeliverInterrupts();
	}
}

extern "C" void ps3WaitOn(void* key, const char* reason)
{
	lockS();
	if (sInInterrupt) {
		unlockS();
		ps3Log("[ps3] FAILED: blocking wait (%s) inside an interrupt\n", reason);
		return;
	}
	blockLocked(sRunning, key, reason);
	unlockS();
}

extern "C" void ps3WakeAll(void* key)
{
	lockS();
	wakeAllLocked(key);
	preemptCheck(sRunning);
	unlockS();
}

BOOL OSDisableInterrupts(void)
{
	BOOL prev    = sIntrEnabled;
	sIntrEnabled = 0;
	return prev;
}

BOOL OSRestoreInterrupts(BOOL level)
{
	BOOL prev    = sIntrEnabled;
	sIntrEnabled = level ? 1 : 0;
	if (level && !prev) {
		ps3DeliverInterrupts();
	}
	return prev;
}

static __OSInterruptHandler sHandlers[32];

__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler)
{
	if (interrupt < 0 || interrupt >= 32) {
		return NULL;
	}
	__OSInterruptHandler old = sHandlers[interrupt];
	sHandlers[interrupt]     = handler;
	return old;
}

// Raises a GameCube hardware interrupt: its registered handler (__OSSetInterruptHandler) runs like an
// interrupt, at the next delivery point.
static OSContext sInterruptContext;

static void runInterruptHandler(void* arg)
{
	int interrupt              = (int)(uptr)arg;
	__OSInterruptHandler handler = (interrupt >= 0 && interrupt < 32) ? sHandlers[interrupt] : NULL;
	if (handler) {
		handler((__OSInterrupt)interrupt, &sInterruptContext);
	}
}

extern "C" void ps3RaiseInterrupt(int interrupt)
{
	ps3PostInterrupt(runInterruptHandler, (void*)(uptr)interrupt);
}

OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask)
{
	(void)mask;
	return 0;
}

// ------------------------------------------------------------------ threads

static ThreadImpl* allocImpl(OSThread* t)
{
	ThreadImpl* impl = findImpl(t);
	if (impl) {
		return impl; // the game reuses OSThread objects (e.g. the loading thread)
	}
	if (sImplCount >= MAX_THREADS) {
		return NULL;
	}
	impl        = &sImpls[sImplCount];
	impl->index = sImplCount;
	ps3b_cond_create(&impl->cv, sLock);
	sImplCount++;
	impl->os = t;
	return impl;
}

extern "C" void ps3SchedInit(void)
{
	ps3b_mutex_create(&sLock);
	ps3b_cond_create(&sIdleCv, sLock);
	memset(&sDefaultThread, 0, sizeof(sDefaultThread));
	sDefaultThread.state    = OS_THREAD_STATE_RUNNING;
	sDefaultThread.priority = 16; // the GameCube default thread's priority
	sDefaultThread.base     = 16;
	lockS();
	ThreadImpl* main = allocImpl(&sDefaultThread);
	unsigned long long lo, hi;
	ps3b_thread_stack(&lo, &hi);
	setStack(main, lo, hi);
	// (the page check must say yes for this thread's own stack, or it is not used)
	volatile int probe = 0;
	if (!ps3b_address_readable((unsigned long long)(uptr)&probe)) {
		sCheckPages = 0;
	}
	unlockS();
	ps3Log("[ps3] stack page checks %s\n", sCheckPages ? "on" : "OFF (sys_memory_get_page_attribute did not work)");
	lockS();
	main->started     = 1;
	main->hasBaton    = 1;
	main->intrEnabled = 1; // restored into sIntrEnabled whenever main gets the baton back
	sRunning         = main;
	unlockS();
}

void OSInitThreadQueue(OSThreadQueue* queue)
{
	queue->head = NULL;
	queue->tail = NULL;
}

OSThread* OSGetCurrentThread(void)
{
	// Before ps3SchedInit (static constructors run before main on the PS3, but after OSInit on the
	// GameCube) the caller is the main thread: return the default thread object, set up later in place.
	return sRunning ? sRunning->os : &sDefaultThread;
}

BOOL OSCreateThread(OSThread* thread, OSThreadStartFunction func, void* param, void* stack, u32 stackSize, OSPriority priority, u16 attr)
{
	PS3_ACTIVITY("OSCreateThread");
	if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX) {
		return FALSE;
	}
	lockS();
	ThreadImpl* impl = allocImpl(thread);
	if (!impl) {
		unlockS();
		ps3Log("[ps3] FAILED: too many threads\n");
		return FALSE;
	}
	// The game reuses OSThread objects: a thread that was cancelled may still be on its PS3 thread, which
	// has yet to see that (it needs sLock). Its worker lets go of it first, or two PS3 threads would run the
	// new one.
	for (int waited = 0; impl->worker && impl != sRunning && (impl->cancelled || impl->exiting); waited++) {
		if (waited == 10000) {
			ps3Log("[ps3] warning: OSCreateThread: thread %d has not left its PS3 thread after 1 s\n", impl->index);
			break;
		}
		unlockS();
		ps3b_sleep_us(100);
		lockS();
	}
	if (impl->started && impl != sRunning && !impl->exiting && !impl->cancelled && thread->state != 0
	    && thread->state != OS_THREAD_STATE_MORIBUND) {
		ps3Log("[ps3] warning: OSCreateThread on a live thread object (thread %d)\n", impl->index);
	}
	memset(thread, 0, sizeof(OSThread));
	thread->state     = OS_THREAD_STATE_READY;
	thread->suspend   = 1; // created suspended, like the GameCube
	thread->priority  = priority;
	thread->base      = priority;
	thread->attr      = attr;
	thread->stackBase = (u8*)stack;
	thread->stackEnd  = (u32*)((u8*)stack - stackSize);
	impl->func        = func;
	impl->arg         = param;
	impl->started     = 0;
	impl->exiting     = 0;
	impl->cancelled   = 0;
	impl->hasBaton    = 0;
	impl->intrEnabled = 1;
	impl->waitKey     = NULL;
	memset(&impl->hook, 0, sizeof(impl->hook));
	unlockS();
	ps3Log("[ps3] thread %d created (priority %d)\n", impl->index, (int)priority);
	return TRUE;
}

s32 OSResumeThread(OSThread* thread)
{
	PS3_ACTIVITY("OSResumeThread");
	lockS();
	ThreadImpl* impl = findImpl(thread);
	s32 prev         = thread->suspend;
	if (impl && prev > 0) {
		thread->suspend--;
		if (thread->suspend == 0 && thread->state == OS_THREAD_STATE_READY) {
			makeReady(impl, 0);
		}
	}
	preemptCheck(sRunning);
	unlockS();
	return prev;
}

void OSExitThread(void* val)
{
	lockS();
	ThreadImpl* self = sRunning;
	self->os->val    = val;
	self->os->state  = (self->os->attr & OS_THREAD_ATTR_DETACH) ? 0 : OS_THREAD_STATE_MORIBUND;
	self->exiting    = 1;
	wakeAllLocked(&self->os->queueJoin);
	if (self->os == &sDefaultThread) {
		unlockS();
		ps3Log("[ps3] main thread exited\n");
		ps3b_exit(0);
	}
	schedule(self); // hands the baton on and returns because we are exiting
	leaveThread(self);
}

void OSCancelThread(OSThread* thread)
{
	lockS();
	ThreadImpl* impl = findImpl(thread);
	if (!impl) {
		unlockS();
		return;
	}
	if (impl == sRunning) {
		unlockS();
		OSExitThread(NULL);
		return;
	}
	thread->state   = (thread->attr & OS_THREAD_ATTR_DETACH) ? 0 : OS_THREAD_STATE_MORIBUND;
	impl->cancelled = 1;
	impl->waitKey   = NULL;
	wakeAllLocked(&thread->queueJoin);
	if (impl->started) {
		ps3b_cond_signal(impl->cv);
	}
	preemptCheck(sRunning);
	unlockS();
}

BOOL OSJoinThread(OSThread* thread, void** val)
{
	PS3_ACTIVITY("OSJoinThread");
	lockS();
	ThreadImpl* impl = findImpl(thread);
	if (!impl || (thread->attr & OS_THREAD_ATTR_DETACH)) {
		unlockS();
		return FALSE;
	}
	while (thread->state != OS_THREAD_STATE_MORIBUND && thread->state != 0) {
		blockLocked(sRunning, &thread->queueJoin, "OSJoinThread");
	}
	if (val) {
		*val = thread->val;
	}
	thread->state = 0;
	unlockS();
	return TRUE;
}

void OSYieldThread(void)
{
	ps3DeliverInterrupts();
	lockS();
	ThreadImpl* self  = sRunning;
	ThreadImpl* other = pickReady(self);
	if (other && other->os->priority <= self->os->priority) {
		makeReady(self, 0); // back of its priority's queue
		schedule(self);
	}
	unlockS();
}

// Thread queues (the GX library waits for "draw done" this way): a sleeping thread waits on the queue
// object until OSWakeupThread wakes every thread on it.
void OSSleepThread(OSThreadQueue* queue)
{
	lockS();
	if (sInInterrupt) {
		unlockS();
		ps3Log("[ps3] FAILED: OSSleepThread inside an interrupt\n");
		return;
	}
	blockLocked(sRunning, queue, "OSSleepThread");
	unlockS();
}

void OSWakeupThread(OSThreadQueue* queue)
{
	lockS();
	wakeAllLocked(queue);
	preemptCheck(sRunning);
	unlockS();
}

OSPriority OSGetThreadPriority(OSThread* thread)
{
	return thread->priority;
}

s32 OSCheckActiveThreads(void)
{
	lockS();
	s32 n = 0;
	for (int i = 0; i < sImplCount; i++) {
		if (sImpls[i].os && sImpls[i].os->state != 0) {
			n++;
		}
	}
	unlockS();
	return n;
}

void OSSetCurrentContext(OSContext* context) { (void)context; }
void OSClearContext(OSContext* context) { (void)context; }

// ------------------------------------------------------------------ message queues

void OSInitMessageQueue(OSMessageQueue* queue, OSMessage* msgArray, s32 msgCount)
{
	OSInitThreadQueue(&queue->queueSend);
	OSInitThreadQueue(&queue->queueReceive);
	queue->msgArray   = msgArray;
	queue->msgCount   = msgCount;
	queue->firstIndex = 0;
	queue->usedCount  = 0;
}

BOOL OSSendMessage(OSMessageQueue* queue, OSMessage msg, s32 flags)
{
	PS3_ACTIVITY("OSSendMessage");
	ps3DeliverInterrupts();
	lockS();
	if (queue->msgCount <= 0) {
		unlockS();
		return FALSE;
	}
	while (queue->usedCount >= queue->msgCount) {
		if (!(flags & OS_MESSAGE_BLOCK) || sInInterrupt) {
			unlockS();
			return FALSE;
		}
		blockLocked(sRunning, &queue->queueSend, "OSSendMessage (queue full)");
	}
	queue->msgArray[(queue->firstIndex + queue->usedCount) % queue->msgCount] = msg;
	queue->usedCount++;
	wakeAllLocked(&queue->queueReceive);
	preemptCheck(sRunning);
	unlockS();
	return TRUE;
}

BOOL OSReceiveMessage(OSMessageQueue* queue, OSMessage* msgPtr, s32 flags)
{
	PS3_ACTIVITY("OSReceiveMessage");
	ps3DeliverInterrupts();
	lockS();
	if (queue->msgCount <= 0) {
		unlockS();
		return FALSE;
	}
	while (queue->usedCount == 0) {
		if (!(flags & OS_MESSAGE_BLOCK) || sInInterrupt) {
			unlockS();
			return FALSE;
		}
		blockLocked(sRunning, &queue->queueReceive, "OSReceiveMessage (queue empty)");
	}
	if (msgPtr) {
		*msgPtr = queue->msgArray[queue->firstIndex];
	}
	queue->firstIndex = (queue->firstIndex + 1) % queue->msgCount;
	queue->usedCount--;
	wakeAllLocked(&queue->queueSend);
	preemptCheck(sRunning);
	unlockS();
	return TRUE;
}

// ------------------------------------------------------------------ mutexes and condition variables

void OSInitMutex(OSMutex* mutex)
{
	OSInitThreadQueue(&mutex->queue);
	mutex->thread = NULL;
	mutex->count  = 0;
}

void OSLockMutex(OSMutex* mutex)
{
	PS3_ACTIVITY("OSLockMutex");
	lockS();
	OSThread* me = sRunning->os;
	while (mutex->thread && mutex->thread != me) {
		blockLocked(sRunning, &mutex->queue, "OSLockMutex");
	}
	mutex->thread = me;
	mutex->count++;
	unlockS();
}

void OSUnlockMutex(OSMutex* mutex)
{
	lockS();
	if (mutex->thread == sRunning->os && --mutex->count <= 0) {
		mutex->count  = 0;
		mutex->thread = NULL;
		wakeAllLocked(&mutex->queue);
		preemptCheck(sRunning);
	}
	unlockS();
}

void OSInitCond(OSCond* cond)
{
	OSInitThreadQueue(&cond->queue);
}

void OSWaitCond(OSCond* cond, OSMutex* mutex)
{
	PS3_ACTIVITY("OSWaitCond");
	lockS();
	OSThread* me = sRunning->os;
	if (mutex->thread != me) {
		unlockS();
		return;
	}
	int count     = mutex->count;
	mutex->count  = 0;
	mutex->thread = NULL;
	wakeAllLocked(&mutex->queue);
	blockLocked(sRunning, &cond->queue, "OSWaitCond");
	while (mutex->thread && mutex->thread != me) {
		blockLocked(sRunning, &mutex->queue, "OSWaitCond (relock)");
	}
	mutex->thread = me;
	mutex->count  = count;
	unlockS();
}

void OSSignalCond(OSCond* cond)
{
	ps3WakeAll(&cond->queue); // the Dolphin SDK wakes every waiter
}

// ------------------------------------------------------------------ watchdog

static const char* stateName(int s)
{
	switch (s) {
	case OS_THREAD_STATE_READY:
		return "ready";
	case OS_THREAD_STATE_RUNNING:
		return "running";
	case OS_THREAD_STATE_WAITING:
		return "waiting";
	case OS_THREAD_STATE_MORIBUND:
		return "ended";
	default:
		return "free";
	}
}

extern "C" void ps3AudioStatus(char* buf, unsigned int size);
extern "C" void ps3GxStatus(char* buf, unsigned int size);
extern "C" void ps3RsxStatus(char* buf, unsigned int size);
extern "C" void ps3GfxPhaseStatus(char* buf, unsigned int size);
extern "C" void ps3AudioProbeStatus(char* buf, unsigned int size);
extern "C" void ps3ProfilerFrameStatus(char* buf, unsigned int size, float msPerFrame);
extern "C" unsigned int ps3FramesDrawn(void);

// `detailed` (stall and exit reports): where the thread is and its stack's bounds are logged before the stack
// is walked, so a report that stops there still says where the thread was.
static void logStack(int index, ThreadImpl* t, bool detailed)
{
	// Innermost frames first. Addresses resolve with tools/ps3/symbolize_log.py and the build's ELF.
	const Ps3HookState& h = hookOf(t);
	void* const pc = h.lastPc;
	void* const sp = h.lastSp;
	if (!pc || !t->started || t->exiting || t->cancelled || !t->os || t->os->state == OS_THREAD_STATE_MORIBUND
	    || t->os->state == 0) {
		return; // (not running game code, or ending)
	}
	unsigned long long lo = 0, hi = 0;
	const bool have = readStack(t, &lo, &hi);
	if (detailed) {
		const Worker* w = t->worker;
		const bool gone = have && sCheckPages && !ps3b_address_readable((unsigned long long)(uptr)sp);
		ps3Log("[stack]   thread %d pc %p sp %p; stack 0x%llx-0x%llx%s, PS3 thread %d%s\n", index, pc, sp, lo, hi,
		       have ? "" : " (changing)", w ? w->index : -1,
		       gone ? "; ITS STACK IS GONE: the thread has crashed (lv2 ends a thread that faults; pc is the last game code"
		              " noted before the crash)"
		            : "");
	}
	void* pcs[24];
	const int depth = have ? ps3WalkStack(sp, lo, hi, pcs, 24) : 0;
	char line[512];
	int n = snprintf(line, sizeof(line), "[stack]   thread %d pc %p sp %p depth %d:", index, pc, sp, depth);
	for (int f = 0; f < depth && n < (int)sizeof(line) - 24; f++) {
		n += snprintf(line + n, sizeof(line) - n, " %p", pcs[f]);
	}
	ps3Log("%s\n", line);
}

extern "C" void ps3ReportStack(const char* what)
{
	ThreadImpl* t = sRunning;
	ps3Log("[ps3] %s\n", what);
	if (t) {
		logStack((int)(t - sImpls), t, true);
	}
}

static void reportThreads(const char* title, bool detailed)
{
	char audio[1024];
	ps3AudioStatus(audio, sizeof(audio));
	char nans[40] = "";
	if (PS3_TRACE_NAN) {
		snprintf(nans, sizeof(nans), "; NaNs created: %u", (unsigned)sNanCount);
	}
	char waits[160];
	const char* what = sWorstWaitWhat;
	snprintf(waits, sizeof(waits), "longest interrupt wait %u ms (thread %d, after \"%s\"), %u over 10 ms",
	         (unsigned)(sWorstWaitTb * 1000 / ps3b_timebase_freq()), sWorstWaitThread, what ? what : "-", (unsigned)sLongWaits);
	sWorstWaitTb     = 0;
	sWorstWaitWhat   = NULL;
	sWorstWaitThread = -1;
	sLongWaits       = 0;
	ps3Log("[ps3] %s: last SDK call \"%s\", %u retraces, %u frames drawn; interrupts %s%s, %u pending, %u dropped, %s; %s%s\n",
	       title, sLastActivity, ps3FrameCounter(), ps3FramesDrawn(), sIntrEnabled ? "on" : "OFF",
	       sInInterrupt ? " (in handler)" : "", (unsigned)(sIntrTail - sIntrHead), (unsigned)sIntrDropped, waits, audio, nans);
	char gfx[1600];
	ps3GxStatus(gfx, sizeof(gfx));
	ps3Log("[ps3]   %s\n", gfx);
	ps3RsxStatus(gfx, sizeof(gfx));
	ps3Log("[ps3]   %s\n", gfx);
	ps3GfxPhaseStatus(gfx, sizeof(gfx));
	ps3Log("[ps3]   %s\n", gfx);
	ps3AudioProbeStatus(gfx, sizeof(gfx));
	ps3Log("[ps3]   %s\n", gfx);
	{
		// (retraces at 59.94 Hz)
		static u32 sLastRetraces, sLastFrames;
		const u32 retraces = ps3FrameCounter(), frames = ps3FramesDrawn();
		const u32 dr = retraces - sLastRetraces, df = frames - sLastFrames;
		sLastRetraces = retraces;
		sLastFrames   = frames;
		ps3ProfilerFrameStatus(gfx, sizeof(gfx), df ? (float)dr * 16.6833f / (float)df : 0.0f);
		ps3Log("[ps3]   %s\n", gfx);
	}
	ps3b_log_memory(title);
	for (int i = 0; i < sImplCount; i++) {
		ThreadImpl* t = &sImpls[i];
		if (!t->os) {
			continue;
		}
		ps3Log("[ps3]   thread %d prio %d %s%s%s%s\n", i, (int)t->os->priority, stateName(t->os->state), t->waitReason ? " in " : "",
		       t->waitReason ? t->waitReason : "", t == sRunning ? "  <- has the baton" : "");
		logStack(i, t, detailed);
	}
	if (detailed) {
		ps3Log("[ps3]   PS3 threads for game threads: %d made, %d waiting for one; the profiler has sampled %u times; stack page "
		       "checks %s%s\n",
		       sWorkerCount, sIdleWorkerCount, ps3ProfilerTicks(), sCheckPages ? "on" : "OFF (not available)",
		       sStackGone ? "; a stack walk found a thread's stack gone" : "");
	}
}

// When the game quits (test build 48): the state of every thread, as in a stall report. Test 47's game froze
// while loading a level; the watchdog had stopped before it could report, and the exit was all that ran.
extern "C" void ps3SchedExitReport(void)
{
	reportThreads("exit", true);
}

static void watchdogThread(void*)
{
	u32 lastActivity = sActivity;
	u32 lastMoves    = 0;
	u32 lastTicks    = 0;
	void* lastPc     = NULL;
	void* lastSp     = NULL;
	ThreadImpl* lastRunner = NULL;
	int idleSdk      = 0;
	int idleBlocks   = 0;
	int profilerIdle = 0;
	int seconds      = 0;
	bool goneReported = false;
	for (;;) {
		ps3b_sleep_us(1000 * 1000);
		seconds++;
		u32 now = sActivity;
		idleSdk = (now == lastActivity) ? idleSdk + 1 : 0;
		lastActivity = now;

		// The baton holder is executing game code but no game function has started or returned (the
		// profiler, which looks 250 times a second, saw it at the same place every time): a loop without
		// calls, or the thread has stopped (lv2 halts a thread that faults). Test build 48: if the profiler
		// itself has stopped sampling, the watchdog's own look once a second decides instead, and the
		// profiler's stop is reported.
		ThreadImpl* runner = sRunning;
		const u32 moves    = ps3ProfilerMoves();
		const u32 ticks    = ps3ProfilerTicks();
		profilerIdle       = (ticks != 0 && ticks == lastTicks) ? profilerIdle + 1 : 0; // (0: not started yet)
		lastTicks          = ticks;
		void* const pc     = ps3Hook.lastPc;
		void* const sp     = ps3Hook.lastSp;
		int running        = runner && runner->os && runner->os->state == OS_THREAD_STATE_RUNNING;
		int same           = running && runner == lastRunner;
		int still          = profilerIdle ? (pc == lastPc && sp == lastSp) : (moves == lastMoves);
		idleBlocks         = (same && still) ? idleBlocks + 1 : 0;
		lastRunner         = runner;
		lastMoves          = moves;
		lastPc             = pc;
		lastSp             = sp;

		if (sStackGone && !goneReported) {
			goneReported = true;
			reportThreads("CRASH: a game thread's stack is gone (lv2 ends a thread that faults)", true);
		} else if (profilerIdle == 3) {
			reportThreads("STALL: the profiler has stopped sampling (for 3 s)", true);
		} else if (idleBlocks == 5 || (idleBlocks > 0 && idleBlocks % 30 == 0)) {
			char title[128];
			snprintf(title, sizeof(title), "STALL: thread %d stuck in one place for %d s (crashed, blocked outside game code, or looping without calls)",
			         runner->index, idleBlocks);
			reportThreads(title, true);
		} else if (idleSdk == 5 || (idleSdk > 0 && idleSdk % 30 == 0)) {
			char title[64];
			snprintf(title, sizeof(title), "STALL: no SDK activity for %d s", idleSdk);
			reportThreads(title, true);
		} else if (seconds % 15 == 0) {
			reportThreads("heartbeat", false);
		}
	}
}

extern "C" void ps3StartWatchdog(void)
{
	ps3b_thread_create(watchdogThread, NULL, 1000, 0x10000, "pikmin_watchdog");
}
