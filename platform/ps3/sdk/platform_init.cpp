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

// diagnostics.txt in the PIKMIN folder on the USB drive (or the game's folder) turns on the testing extras (see
// sdk_internal.h).
extern "C" int ps3DiagnosticsOn(void)
{
	static int sOn = -1;
	if (sOn < 0) {
		const char* root = ps3b_work_dir();
		char path[320];
		sOn = 0;
		if (root) {
			snprintf(path, sizeof(path), "%s/diagnostics.txt", root);
			sOn = ps3b_file_exists(path) != 0;
		}
	}
	return sOn;
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
	ps3Log("[boot] Pikmin PS3 port, build %d (built %s %s)\n", PS3_TEST_BUILD, __DATE__, __TIME__);
	const char* logPath = ps3b_log_path();
	ps3Log("[boot] log file: %s\n", logPath ? logPath : "none (nowhere to write it)");
	if (const char* copy = ps3b_log_copy_path()) {
		ps3Log("[boot] log file copy: %s\n", copy);
	}
	ps3b_log_memory("start");

	ps3b_console_init();
	// (as early as possible: playing a faint noise until the game's sound starts, so that the TV or sound
	// system is listening by the time of the Nintendo logo's sound)
	ps3BootStage("sound output");
	ps3AudioStartOutput();

	ps3BootStage("scheduler");
	ps3SchedInit();
	ps3Log("[boot] diagnostics: %s\n", ps3DiagnosticsOn() ? "on (PIKMIN/diagnostics.txt): profiler, frame recordings, heartbeats every 15 s"
	                                                       : "off (crash and stall reports only; add PIKMIN/diagnostics.txt to turn them on)");
	ps3ProfilerStart(ps3DiagnosticsOn());

	ps3BootStage("looking for the game data");
	if (!ps3b_prepare_game_data()) {
		const char* usrdir = ps3b_usrdir();
		ps3Log("[ps3] FAILED: the game data was not found.\n");
		ps3Log("[ps3] This port needs your own Pikmin disc image (North America, GPIE01), as a .iso, .gcm or .ciso file.\n");
		ps3Log("[ps3] Either put it in %s with a file manager or FTP,\n", usrdir ? usrdir : "the game's folder on the hard drive");
		ps3Log("[ps3] or copy it to a USB drive (at the top, or in a PIKMIN folder) for the game to copy on its next start.\n");
		ps3Log("[ps3] Press the PS button and choose Quit Game.\n");
		for (;;) {
			ps3b_sleep_us(1000 * 1000);
		}
	}

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
