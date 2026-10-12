// PS3 port: platform start-up, called from main() before the game's own initialisation.

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#include "ps3/platform.h"

extern "C" void ps3OsMiscInit(void);
extern "C" void ps3StartWatchdog(void);

extern "C" void ps3PlatformInit(void)
{
	static int sDone;
	if (sDone) {
		return;
	}
	sDone = 1;

	ps3b_init();
	ps3b_console_init();
	ps3SchedInit();
	ps3Log("Pikmin PS3 port - Phase 3 boot test (graphics and sound are not implemented yet)\n");

	const char* root = ps3b_data_root();
	if (!root) {
		ps3Log("[ps3] FAILED: game data not found.\n");
		ps3Log("[ps3] Copy the PIKMIN folder (containing fst.txt and files/) to the top level of a USB drive,\n");
		ps3Log("[ps3] plug it in, and start the game again.\n");
		for (;;) {
			ps3b_sleep_us(1000 * 1000);
		}
	}
	ps3Log("[ps3] game data: %s (log file: %s/pikmin_log.txt)\n", root, root);

	ps3OsMiscInit();
	ps3DvdInit();
	ps3ViStart();
	ps3StartWatchdog();
	ps3Log("[ps3] platform ready, starting the game\n");
}
