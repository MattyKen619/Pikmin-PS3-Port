// PS3 port: Dolphin OS threads, message queues, mutexes, condition variables and interrupts.
//
// The GameCube runs exactly one thread at a time, always the highest-priority ready one, and only
// switches threads when the running one blocks, yields, or an interrupt wakes a higher-priority thread.
// Pikmin relies on this (shared data without locks, OSDisableInterrupts as a critical section).
//
// Each OSThread gets a real PS3 thread, but only the holder of a "baton" executes game code. Blocking
// calls hand the baton to the best ready thread. GameCube interrupts (60 Hz retrace, I/O completion)
// are queued by background threads and run by the baton holder at the next scheduling point, or by
// the idle loop when every game thread is waiting.

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MAX_THREADS     32
#define GAME_LV2_PRIO   1500    // all game threads share one lv2 priority; the baton decides who runs
#define GAME_STACK_SIZE 0x40000 // 256 KB: 64-bit code uses more stack than the GameCube build did
#define MAX_INTERRUPTS  256
#define SHADOW_DEPTH    128

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

	// Shadow call stack, kept by the -finstrument-functions hooks (see "preemption points" below).
	void* stackFn[SHADOW_DEPTH];
	void* stackSite[SHADOW_DEPTH];
	volatile int depth;
	volatile u32 calls;
	u32 reportedCalls;

	// Basic-block trace (-fsanitize-coverage=trace-pc): the last game-code address this thread ran.
	void* volatile lastPc;
	volatile u32 blocks;
	u32 reportedBlocks;
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
static u32 sSeq;
static volatile int sIntrEnabled = 1;
static volatile int sInInterrupt = 0;
static OSThread sDefaultThread;

static Interrupt sIntr[MAX_INTERRUPTS];
static volatile u32 sIntrHead, sIntrTail;
static volatile u32 sIntrDropped;

static volatile u32 sActivity;
static const char* volatile sLastActivity = "boot";

static void lockS() { ps3b_mutex_lock(sLock); }
static void unlockS() { ps3b_mutex_unlock(sLock); }

// ------------------------------------------------------------------ logging

extern "C" void ps3Log(const char* fmt, ...)
{
	char buf[1024];
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

static void waitBaton(ThreadImpl* self)
{
	while (!self->hasBaton && !self->cancelled) {
		ps3b_cond_wait(self->cv, 100000);
	}
	if (self->cancelled && !self->hasBaton) {
		unlockS();
		ps3b_thread_exit();
	}
}

static void threadEntry(void* p);

static void startPs3Thread(ThreadImpl* t)
{
	char name[24];
	snprintf(name, sizeof(name), "pikmin_os%d", t->index);
	t->started = 1;
	if (ps3b_thread_create(threadEntry, t, GAME_LV2_PRIO, GAME_STACK_SIZE, name) != 0) {
		ps3Log("[ps3] FAILED to create PS3 thread for OS thread %d\n", t->index);
	}
}

// Runs queued interrupts on behalf of `self` (the baton holder or the idle loop). Returns how many ran.
static int deliverLocked(ThreadImpl* self, int force)
{
	(void)self;
	if (sInInterrupt || (!force && !sIntrEnabled)) {
		return 0;
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

static void threadEntry(void* p)
{
	ThreadImpl* self = (ThreadImpl*)p;
	lockS();
	waitBaton(self);
	unlockS();
	ps3NoteActivity("thread start");
	void* result = self->func(self->arg);
	OSExitThread(result);
}

// ------------------------------------------------------------------ interrupts

extern "C" void ps3PostInterrupt(Ps3InterruptFn fn, void* arg)
{
	lockS();
	if (sIntrTail - sIntrHead >= MAX_INTERRUPTS) {
		sIntrDropped++;
	} else {
		sIntr[sIntrTail % MAX_INTERRUPTS].fn  = fn;
		sIntr[sIntrTail % MAX_INTERRUPTS].arg = arg;
		sIntrTail++;
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
	void* pc = t->lastPc;
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

extern "C" void __cyg_profile_func_enter(void* fn, void* site)
{
	ThreadImpl* t = sRunning;
	if (!t) {
		return; // static constructors, before the scheduler starts
	}
	CHECK_NAN_ORIGIN(t); // before pushing: the caller is the function that ran last
	int d = t->depth;
	if (d < SHADOW_DEPTH) {
		t->stackFn[d]   = fn;
		t->stackSite[d] = site;
	}
	t->depth = d + 1;
	t->calls++;
	if (sIntrHead != sIntrTail && sIntrEnabled && !sInInterrupt) {
		void* pc = t->lastPc;
		ps3DeliverInterrupts();
		t->lastPc = pc; // interrupt handlers ran on this thread: point back at the interrupted code
	}
}

extern "C" void __cyg_profile_func_exit(void* fn, void* site)
{
	(void)fn;
	(void)site;
	ThreadImpl* t = sRunning;
	if (t) {
		CHECK_NAN_ORIGIN(t); // before popping: the returning function is still on top
		if (t->depth > 0) {
			t->depth--;
		}
	}
}

// Called at the start of every basic block of game code (-fsanitize-coverage=trace-pc). Records where
// the thread is, which pinpoints a hang or the last code a crashed thread ran (lv2 stops a thread that
// faults while the rest of the game keeps going), and makes every loop a switch point.
extern "C" void __sanitizer_cov_trace_pc(void) __attribute__((no_instrument_function));
extern "C" void __sanitizer_cov_trace_pc(void)
{
	ThreadImpl* t = sRunning;
	if (!t) {
		return;
	}
	void* pc  = __builtin_return_address(0);
	t->lastPc = pc;
	t->blocks++;
	if (sIntrHead != sIntrTail && sIntrEnabled && !sInInterrupt) {
		ps3DeliverInterrupts();
		t->lastPc = pc; // interrupt handlers ran on this thread: point back at the interrupted code
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
	impl->depth       = 0;
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
	unlockS();
	ps3b_thread_exit();
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
extern "C" unsigned int ps3FramesDrawn(void);

static void logStack(int index, ThreadImpl* t)
{
	// Innermost frames first. Addresses resolve with tools/ps3/symbolize_log.py and the build's ELF.
	int depth = t->depth;
	if (depth > 0) {
		char line[512];
		int n     = snprintf(line, sizeof(line), "[stack]   thread %d pc %p depth %d:", index, t->lastPc, depth);
		int shown = depth < SHADOW_DEPTH ? depth : SHADOW_DEPTH;
		for (int f = shown - 1; f >= 0 && f >= shown - 10 && n < (int)sizeof(line) - 40; f--) {
			n += snprintf(line + n, sizeof(line) - n, " %p<%p", t->stackFn[f], t->stackSite[f]);
		}
		ps3Log("%s\n", line);
	}
}

extern "C" void ps3ReportStack(const char* what)
{
	ThreadImpl* t = sRunning;
	ps3Log("[ps3] %s\n", what);
	if (t) {
		logStack((int)(t - sImpls), t);
	}
}

static void reportThreads(const char* title)
{
	char audio[384];
	ps3AudioStatus(audio, sizeof(audio));
	char nans[40] = "";
	if (PS3_TRACE_NAN) {
		snprintf(nans, sizeof(nans), "; NaNs created: %u", (unsigned)sNanCount);
	}
	ps3Log("[ps3] %s: last SDK call \"%s\", %u retraces, %u frames drawn; interrupts %s%s, %u pending, %u dropped; %s%s\n",
	       title, sLastActivity, ps3FrameCounter(), ps3FramesDrawn(), sIntrEnabled ? "on" : "OFF",
	       sInInterrupt ? " (in handler)" : "", (unsigned)(sIntrTail - sIntrHead), (unsigned)sIntrDropped, audio, nans);
	for (int i = 0; i < sImplCount; i++) {
		ThreadImpl* t = &sImpls[i];
		if (!t->os) {
			continue;
		}
		u32 calls         = t->calls;
		u32 recent        = calls - t->reportedCalls;
		t->reportedCalls  = calls;
		u32 blocks        = t->blocks;
		u32 recentBlocks  = blocks - t->reportedBlocks;
		t->reportedBlocks = blocks;
		ps3Log("[ps3]   thread %d prio %d %s%s%s%s, %u calls, %u blocks since last report\n", i, (int)t->os->priority,
		       stateName(t->os->state), t->waitReason ? " in " : "", t->waitReason ? t->waitReason : "",
		       t == sRunning ? "  <- has the baton" : "", (unsigned)recent, (unsigned)recentBlocks);
		logStack(i, t);
	}
}

static void watchdogThread(void*)
{
	u32 lastActivity = sActivity;
	u32 lastCalls    = 0;
	ThreadImpl* lastRunner = NULL;
	int idleSdk      = 0;
	int idleCalls    = 0;
	int idleBlocks   = 0;
	u32 lastBlocks   = 0;
	int seconds      = 0;
	for (;;) {
		ps3b_sleep_us(1000 * 1000);
		seconds++;
		u32 now = sActivity;
		idleSdk = (now == lastActivity) ? idleSdk + 1 : 0;
		lastActivity = now;

		// The baton holder is executing game code but has not called a function: a loop without calls,
		// or (if not even a basic block ran) the thread has stopped: lv2 halts a thread that faults.
		ThreadImpl* runner = sRunning;
		u32 calls          = runner ? runner->calls : 0;
		u32 blocks         = runner ? runner->blocks : 0;
		int running        = runner && runner->os && runner->os->state == OS_THREAD_STATE_RUNNING;
		int same           = running && runner == lastRunner;
		idleCalls          = (same && calls == lastCalls) ? idleCalls + 1 : 0;
		idleBlocks         = (same && blocks == lastBlocks) ? idleBlocks + 1 : 0;
		lastRunner         = runner;
		lastCalls          = calls;
		lastBlocks         = blocks;

		if (idleBlocks == 5 || (idleBlocks > 0 && idleBlocks % 30 == 0)) {
			char title[112];
			snprintf(title, sizeof(title), "STALL: thread %d stopped for %d s (crashed, or blocked outside game code)",
			         runner->index, idleBlocks);
			reportThreads(title);
		} else if (idleCalls == 5 || (idleCalls > 0 && idleCalls % 30 == 0)) {
			char title[80];
			snprintf(title, sizeof(title), "STALL: thread %d looping without calls for %d s", runner->index, idleCalls);
			reportThreads(title);
		} else if (idleSdk == 5 || (idleSdk > 0 && idleSdk % 30 == 0)) {
			char title[64];
			snprintf(title, sizeof(title), "STALL: no SDK activity for %d s", idleSdk);
			reportThreads(title);
		} else if (seconds % 15 == 0) {
			reportThreads("heartbeat");
		}
	}
}

extern "C" void ps3StartWatchdog(void)
{
	ps3b_thread_create(watchdogThread, NULL, 1000, 0x10000, "pikmin_watchdog");
}
