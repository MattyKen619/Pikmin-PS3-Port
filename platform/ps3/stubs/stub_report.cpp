// PS3 port: where stub reports go. For now plain stdout (the PS3 toolchain's newlib sends stdout to
// the lv2 TTY); Phase 3 routes this through the OSReport replacement and a log file.
#include "ps3/stub.h"

#include <stdio.h>

extern "C" void ps3StubReport(const char* name)
{
	printf("[ps3 stub] %s called (not implemented yet)\n", name);
}
