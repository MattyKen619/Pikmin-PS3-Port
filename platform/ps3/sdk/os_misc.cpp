// PS3 port: Dolphin OS basics - init, arena and heap, time, reporting, reset, cache operations.

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// The game builds its own heap over the whole OS arena. The GameCube had ~24 MB; 64-bit pointers make
// many runtime objects larger, so the PS3 build gives it plenty of headroom.
#define ARENA_SIZE (96 * 1024 * 1024)

#define GC_TIMER_HZ     40500000ULL // OS_TIMER_CLOCK = 162 MHz bus / 4 (see sdk_globals.cpp)
#define GC_EPOCH_OFFSET 946684800LL // seconds from 1970-01-01 to 2000-01-01 (GameCube epoch)

static u8* sArenaLo;
static u8* sArenaHi;
static unsigned long long sTb0;
static unsigned long long sTbFreq;
static long long sEpoch0;

extern "C" void ps3OsMiscInit(void)
{
	sArenaLo = (u8*)ps3b_memalign(32, ARENA_SIZE);
	sArenaHi = sArenaLo ? sArenaLo + ARENA_SIZE : NULL;
	sTb0     = ps3b_timebase();
	sTbFreq  = ps3b_timebase_freq();
	sEpoch0  = ps3b_epoch_seconds();
	if (!sArenaLo) {
		ps3Log("[ps3] FAILED to allocate the %d MB OS arena\n", ARENA_SIZE >> 20);
	}
}

void OSInit(void)
{
	ps3NoteActivity("OSInit");
	ps3Log("[ps3] OSInit: arena %p-%p (%d MB), timebase %llu Hz\n", sArenaLo, sArenaHi, ARENA_SIZE >> 20, sTbFreq);
}

void* OSGetArenaLo(void) { return sArenaLo; }
void* OSGetArenaHi(void) { return sArenaHi; }

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps)
{
	(void)arenaEnd;
	// Reserve room for the heap descriptor table, as the SDK does, and return the new arena start.
	uptr p = (uptr)arenaStart + (uptr)(maxHeaps > 0 ? maxHeaps : 1) * 0x20;
	return (void*)((p + 31) & ~(uptr)31);
}

void* OSAllocFromHeap(OSHeapHandle heap, u32 size)
{
	static int sLogged;
	ps3LogOnce(&sLogged, "[ps3] OSAllocFromHeap(%d, %u): served from the PS3 heap\n", (int)heap, (unsigned)size);
	return ps3b_memalign(32, size);
}

// ------------------------------------------------------------------ time

static unsigned long long elapsedGcTicks(void)
{
	unsigned long long d = ps3b_timebase() - sTb0;
	return (d / sTbFreq) * GC_TIMER_HZ + ((d % sTbFreq) * GC_TIMER_HZ) / sTbFreq;
}

OSTick OSGetTick(void)
{
	return (OSTick)elapsedGcTicks();
}

OSTime OSGetTime(void)
{
	long long secsSince2000 = sEpoch0 - GC_EPOCH_OFFSET;
	return (OSTime)(secsSince2000 * (long long)GC_TIMER_HZ + (long long)elapsedGcTicks());
}

static int isLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td)
{
	// 40.5 MHz is not a whole number of ticks per microsecond, so convert via whole seconds:
	long long totalUsec = (ticks / (long long)GC_TIMER_HZ) * 1000000LL
	                    + ((ticks % (long long)GC_TIMER_HZ) * 1000000LL) / (long long)GC_TIMER_HZ;
	long long secs     = totalUsec / 1000000LL;
	long long subUsec  = totalUsec % 1000000LL;
	if (subUsec < 0) {
		subUsec += 1000000LL;
		secs--;
	}
	long long days = secs / 86400;
	long long rem  = secs % 86400;
	if (rem < 0) {
		rem += 86400;
		days--;
	}
	td->hour = (int)(rem / 3600);
	td->min  = (int)((rem % 3600) / 60);
	td->sec  = (int)(rem % 60);
	td->msec = (int)(subUsec / 1000);
	td->usec = (int)(subUsec % 1000);
	td->wday = (int)((days + 6) % 7); // 2000-01-01 was a Saturday
	if (td->wday < 0) {
		td->wday += 7;
	}
	int year = 2000;
	while (days < 0) {
		year--;
		days += isLeap(year) ? 366 : 365;
	}
	for (;;) {
		int len = isLeap(year) ? 366 : 365;
		if (days < len) {
			break;
		}
		days -= len;
		year++;
	}
	td->year = year;
	td->yday = (int)days;
	static const int monthLen[2][12] = { { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 },
		                                 { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 } };
	int m = 0;
	while (days >= monthLen[isLeap(year)][m]) {
		days -= monthLen[isLeap(year)][m];
		m++;
	}
	td->mon  = m;
	td->mday = (int)days + 1;
}

// ------------------------------------------------------------------ reporting

void OSReport(const char* fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ps3b_log_write(buf);
}

void OSPanic(const char* file, int line, const char* fmt, ...)
{
	char buf[1024];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ps3Log("[ps3] PANIC in %s line %d: %s\n", file, line, buf);
	ps3Log("[ps3] The game stopped here. Press the PS button and choose Quit Game.\n");
	for (;;) {
		ps3b_sleep_us(1000 * 1000);
	}
}

// ------------------------------------------------------------------ reset and video/sound settings

BOOL OSGetResetSwitchState(void) { return FALSE; }

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)
{
	ps3Log("[ps3] OSResetSystem(%d, 0x%x, %d): returning to the XMB\n", reset, (unsigned)resetCode, (int)forceMenu);
	ps3b_exit(0);
}

static u32 sProgressive = 0;
static u32 sSoundMode   = 1; // stereo

u32 OSGetProgressiveMode(void) { return sProgressive; }
void OSSetProgressiveMode(u32 on) { sProgressive = on; }
u32 OSGetSoundMode(void) { return sSoundMode; }
void OSSetSoundMode(u32 mode) { sSoundMode = mode; }

// ------------------------------------------------------------------ cache control
// The PS3 PPU's caches are coherent for everything this game does, so these are no-ops, except
// DCZeroRange, whose job (zeroing memory) the game relies on.

void DCFlushRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCFlushRangeNoSync(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCInvalidateRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCStoreRange(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCStoreRangeNoSync(void* addr, u32 nBytes) { (void)addr; (void)nBytes; }
void DCZeroRange(void* addr, u32 nBytes) { memset(addr, 0, nBytes); }
void PPCSync(void) { __sync_synchronize(); }
