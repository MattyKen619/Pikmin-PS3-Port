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

static void reportThreads(const char* title)
{
	ps3Log("[ps3] %s: last SDK call \"%s\", %u frames, %u dropped interrupts\n", title, sLastActivity, ps3FrameCounter(),
	       (unsigned)sIntrDropped);
	for (int i = 0; i < sImplCount; i++) {
		ThreadImpl* t = &sImpls[i];
		if (!t->os) {
			continue;
		}
		ps3Log("[ps3]   thread %d prio %d %s%s%s%s\n", i, (int)t->os->priority, stateName(t->os->state), t->waitReason ? " in " : "",
		       t->waitReason ? t->waitReason : "", t == sRunning ? "  <- has the baton" : "");
	}
}

static void watchdogThread(void*)
{
	u32 last     = sActivity;
	int stalled  = 0;
	int seconds  = 0;
	for (;;) {
		ps3b_sleep_us(1000 * 1000);
		seconds++;
		u32 now = sActivity;
		if (now == last) {
			stalled++;
			if (stalled == 5 || stalled % 30 == 0) {
				char title[64];
				snprintf(title, sizeof(title), "STALL: no SDK activity for %d s", stalled);
				reportThreads(title);
			}
		} else {
			stalled = 0;
		}
		last = now;
		if (seconds % 15 == 0) {
			reportThreads("heartbeat");
		}
	}
}

extern "C" void ps3StartWatchdog(void)
{
	ps3b_thread_create(watchdogThread, NULL, 1000, 0x10000, "pikmin_watchdog");
}
