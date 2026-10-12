// PS3 port: where stub reports go - the platform log (TTY, log file and on-screen console).
#include "ps3/stub.h"

#include "../backend/ps3_backend.h"
#include <stdio.h>

extern "C" void ps3StubReport(const char* name)
{
	char line[160];
	snprintf(line, sizeof(line), "[ps3 stub] %s called (not implemented yet)\n", name);
	ps3b_log_write(line);
}
