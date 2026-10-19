// PS3 port: platform start-up, called from main() before the game's own initialisation.
//
// Every step is logged with a "[boot]" line before it runs. The log file is closed after every line,
// so if the console freezes, the last [boot] line in pikmin_log.txt shows the step that failed.

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#include "ps3/platform.h"
#include <stdarg.h>
#include <stdio.h>

extern "C" void ps3OsMiscInit(void);
extern "C" void ps3StartWatchdog(void);
extern "C" void ps3CardPreload(void);
extern "C" void ps3AudioStartOutput(void);

#ifndef PS3_TEST_BUILD
#define PS3_TEST_BUILD 0
#endif

extern "C" void ps3BootStage(const char* what)
{
	ps3Log("[boot] %s\n", what);
}

extern "C" void ps3Report(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ps3Log("%s", buf);
}

// A game heap (AyuHeap) ran out of space. The GameCube build would crash writing to address 0.
extern "C" void ps3HeapExhausted(const char* heapName, int requested, int freeBytes)
{
	ps3Log("[ps3] FAILED: game heap \"%s\" is full: %d bytes requested, %d bytes free\n", heapName ? heapName : "?", requested,
	       freeBytes);
	OSPanic(__FILE__, __LINE__, "out of memory in a game heap");
}

extern "C" void ps3PlatformInit(void)
{
	static int sDone;
	if (sDone) {
		return;
	}
	sDone = 1;

	ps3b_init();
	// The first log line also chooses and creates the log file (before anything that could crash).
	ps3Log("[boot] Pikmin PS3 port, test build %d (built %s %s)\n", PS3_TEST_BUILD, __DATE__, __TIME__);
	const char* logPath = ps3b_log_path();
	ps3Log("[boot] log file: %s\n", logPath ? logPath : "none (no USB drive found)");
	ps3b_log_memory("start");

	ps3b_console_init();
	// (as early as possible: playing a faint noise until the game's sound starts, so that the TV or sound
	// system is listening by the time of the Nintendo logo's sound)
	ps3BootStage("sound output");
	ps3AudioStartOutput();

	ps3BootStage("scheduler");
	ps3SchedInit();
	ps3ProfilerStart();

	ps3BootStage("looking for the game data");
	const char* root = ps3b_data_root();
	if (!root) {
		ps3Log("[ps3] FAILED: game data not found.\n");
		ps3Log("[ps3] Copy the PIKMIN folder (containing fst.txt and files/) to the top level of a USB drive,\n");
		ps3Log("[ps3] plug it in, and start the game again. Hold Start+Select to quit.\n");
		for (;;) {
			ps3b_sleep_us(1000 * 1000);
		}
	}
	ps3Log("[boot] game data: %s\n", root);

	ps3BootStage("OS memory and clock");
	ps3OsMiscInit();
	ps3b_log_memory("after the OS arena");
	ps3BootStage("disc file table");
	ps3DvdInit();
	ps3BootStage("memory card");
	ps3CardPreload();
	ps3BootStage("video timer");
	ps3ViStart();
	ps3BootStage("watchdog");
	ps3StartWatchdog();
	ps3BootStage("platform ready, starting the game");
}
