// PS3 port, host test build: stand-ins for the Dolphin OS and PS3 services that Nintendo's GX library
// (src/gx) and the port's graphics layer (platform/ps3/gpu) use. One thread; interrupts raised by the
// command processor are delivered when interrupts are re-enabled, or when a thread "sleeps".

#include "Dolphin/GX/GXFifo.h"
#include "Dolphin/OS/OSContext.h"
#include "Dolphin/OS/OSInterrupt.h"
#include "Dolphin/OS/OSThread.h"
#include "types.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern "C" {

volatile PPCWGPipe GXWGFifo;
u32 __OSBusClock = 162000000;

// ---- memory: a 96 MB arena at the address the PS3 build gets (above 1 GB), as the port's mapping expects
static u8* sArena;
static const size_t kArenaSize = 96u << 20;

u8* hostArena()
{
	if (!sArena) {
		void* want = (void*)0x52100000;
		void* p    = mmap(want, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
		if (p == MAP_FAILED) {
			fprintf(stderr, "could not map the test arena at %p\n", want);
			exit(2);
		}
		sArena = (u8*)p;
	}
	return sArena;
}

void* OSGetArenaLo(void) { return hostArena(); }
void* OSGetArenaHi(void) { return hostArena() + kArenaSize; }

// ---- interrupts (raised by the graphics thread or the test's own thread, delivered on the test's thread)
static __OSInterruptHandler sHandlers[32];
static int sPending[64];
static int sPendingCount;
static BOOL sEnabled = TRUE;
static OSContext sContext;
static pthread_mutex_t sIntLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sIntCond  = PTHREAD_COND_INITIALIZER;
static pthread_t sMainThread;
static bool sMainKnown;

static bool onMainThread()
{
	if (!sMainKnown) {
		sMainThread = pthread_self();
		sMainKnown  = true;
	}
	return pthread_equal(pthread_self(), sMainThread);
}

static void deliver()
{
	for (;;) {
		pthread_mutex_lock(&sIntLock);
		if (sPendingCount == 0) {
			pthread_mutex_unlock(&sIntLock);
			return;
		}
		int n = sPending[0];
		memmove(sPending, sPending + 1, (sPendingCount - 1) * sizeof(int));
		sPendingCount--;
		pthread_mutex_unlock(&sIntLock);
		if (sHandlers[n]) {
			sHandlers[n]((__OSInterrupt)n, &sContext);
		}
	}
}

void ps3RaiseInterrupt(int interrupt)
{
	pthread_mutex_lock(&sIntLock);
	if (sPendingCount < 64) {
		sPending[sPendingCount++] = interrupt;
	}
	pthread_cond_broadcast(&sIntCond);
	pthread_mutex_unlock(&sIntLock);
	if (sEnabled && onMainThread()) {
		deliver();
	}
}

BOOL OSDisableInterrupts(void)
{
	onMainThread();
	BOOL old = sEnabled;
	sEnabled = FALSE;
	return old;
}

BOOL OSRestoreInterrupts(BOOL level)
{
	BOOL old = sEnabled;
	sEnabled = level;
	if (level) {
		deliver();
	}
	return old;
}

__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler)
{
	__OSInterruptHandler old = sHandlers[interrupt & 31];
	sHandlers[interrupt & 31] = handler;
	return old;
}

OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask) { return mask; }
void OSClearContext(OSContext* context) { (void)context; }
void OSSetCurrentContext(OSContext* context) { (void)context; }

// ---- threads (one)
static OSThread sThread;
static int sSleeps;
OSThread* OSGetCurrentThread(void) { return &sThread; }
void OSInitThreadQueue(OSThreadQueue* queue) { queue->head = queue->tail = NULL; }
void OSSleepThread(OSThreadQueue* queue)
{
	(void)queue;
	// Another thread (or the idle loop) would run the pending interrupts meanwhile: wait for one.
	pthread_mutex_lock(&sIntLock);
	if (sPendingCount == 0) {
		struct timespec t;
		clock_gettime(CLOCK_REALTIME, &t);
		t.tv_sec += 2;
		pthread_cond_timedwait(&sIntCond, &sIntLock, &t);
	}
	bool woken = sPendingCount > 0;
	pthread_mutex_unlock(&sIntLock);
	deliver();
	sSleeps++;
	if (!woken) {
		fprintf(stderr, "FAILED: OSSleepThread: nothing woke the thread for 2 seconds\n");
		exit(1);
	}
}
void OSWakeupThread(OSThreadQueue* queue) { (void)queue; }
s32 OSSuspendThread(OSThread* thread)
{
	(void)thread;
	fprintf(stderr, "FAILED: OSSuspendThread (FIFO overflow handling)\n");
	exit(1);
}
s32 OSResumeThread(OSThread* thread)
{
	(void)thread;
	return 0;
}

OSTime OSGetTime(void)
{
	static OSTime t;
	return t += 1000;
}

void OSPanic(const char* file, int line, const char* message, ...)
{
	fprintf(stderr, "PANIC %s:%d: %s\n", file, line, message);
	exit(1);
}

void PPCSync(void) { }
u32 PPCMfwpar(void) { return 0; }
void PPCMtwpar(u32 value) { (void)value; }
u32 VIGetTvFormat(void) { return 0; }

// ---- PS3 platform services
void ps3Log(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

void ps3LogOnce(int* flag, const char* fmt, ...)
{
	if (*flag) {
		return;
	}
	*flag = 1;
	va_list args;
	va_start(args, fmt);
	vprintf(fmt, args);
	va_end(args);
}

void ps3NoteActivity(const char* what) { (void)what; }
void ps3ProfileSection(const char* name) { (void)name; }

// mutexes and condition variables (handles index these tables)
static pthread_mutex_t sMutexes[16];
static pthread_cond_t sConds[16];
static unsigned sMutexCount, sCondCount;
int ps3b_mutex_create(unsigned int* out)
{
	pthread_mutex_init(&sMutexes[sMutexCount], NULL);
	*out = sMutexCount++;
	return 0;
}
void ps3b_mutex_lock(unsigned int m) { pthread_mutex_lock(&sMutexes[m]); }
void ps3b_mutex_unlock(unsigned int m) { pthread_mutex_unlock(&sMutexes[m]); }
static unsigned sCondMutex[16];
int ps3b_cond_create(unsigned int* out, unsigned int mutex)
{
	pthread_cond_init(&sConds[sCondCount], NULL);
	sCondMutex[sCondCount] = mutex;
	*out                   = sCondCount++;
	return 0;
}
int ps3b_cond_wait(unsigned int c, unsigned int timeout_usec)
{
	struct timespec t;
	clock_gettime(CLOCK_REALTIME, &t);
	t.tv_nsec += (long)(timeout_usec % 1000000) * 1000;
	t.tv_sec += timeout_usec / 1000000 + t.tv_nsec / 1000000000;
	t.tv_nsec %= 1000000000;
	return pthread_cond_timedwait(&sConds[c], &sMutexes[sCondMutex[c]], &t) == 0 ? 0 : 1;
}
void ps3b_cond_signal(unsigned int c) { pthread_cond_signal(&sConds[c]); }
void ps3b_cond_broadcast(unsigned int c) { pthread_cond_broadcast(&sConds[c]); }
int ps3b_file_exists(const char* path) { return access(path, F_OK) == 0; }

const char* ps3b_data_root(void) { return getenv("GX_TRACE_DIR"); }
void* ps3b_memalign(unsigned int align, unsigned int size)
{
	void* p = NULL;
	return posix_memalign(&p, align, size) == 0 ? p : NULL;
}
void ps3b_sleep_us(unsigned int usec) { usleep(usec); }
int ps3b_thread_create(void (*entry)(void*), void* arg, int priority, unsigned int stackSize, const char* name)
{
	(void)priority;
	(void)stackSize;
	(void)name;
	pthread_t t;
	struct Start {
		void (*entry)(void*);
		void* arg;
	};
	Start* s = new Start{ entry, arg };
	return pthread_create(&t, NULL, [](void* p) -> void* {
		Start* s = (Start*)p;
		s->entry(s->arg);
		return NULL;
	}, s) == 0 ? 0 : -1;
}
int ps3b_file_open_rw(const char* path, int create)
{
	return open(path, O_RDWR | (create ? O_CREAT | O_TRUNC : 0), 0644);
}
int ps3b_file_pwrite(int fd, const void* buf, unsigned long long offset, unsigned int len)
{
	return (int)pwrite(fd, buf, len, (off_t)offset);
}
void ps3b_file_close(int fd) { close(fd); }

} // extern "C"

extern "C" unsigned long long ps3b_timebase(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 79800000ULL + (unsigned long long)t.tv_nsec * 798 / 10000;
}
extern "C" unsigned long long ps3b_timebase_freq(void) { return 79800000ULL; }

// ---- the RSX renderer: none on the host (gx_rsx.cpp runs it with a software device instead)
#include "../../../platform/ps3/rsx/rsx_port.h"

gpu::Listener* ps3rsx::start(gpu::CommandProcessor& cp, gpu::Memory& memory)
{
	(void)cp;
	(void)memory;
	return NULL;
}
void ps3rsx::captureFrame(const gpu::CopyInfo& info, const char* name)
{
	(void)info;
	(void)name;
}
void ps3rsx::sectionChanged() { }
void ps3rsx::status(char* buf, unsigned int size)
{
	if (size) {
		buf[0] = 0;
	}
}
