// PS3 port: dvd_test's door into the port's DVD layer (platform/ps3/sdk/dvd.cpp), compiled like it with the
// game's headers. See dvd_test.cpp.

#include "Dolphin/dvd.h"

static DVDFileInfo sInfo;
static volatile s32 sAsyncResult;

static void asyncDone(s32 result, DVDFileInfo*) { sAsyncResult = result; }

extern "C" {
void shimInit(void) { DVDInit(); }
int shimPathToEntry(const char* path) { return DVDConvertPathToEntrynum(path); }
int shimOpen(const char* path, unsigned int* length)
{
	if (!DVDOpen(path, &sInfo)) {
		return 0;
	}
	*length = sInfo.length;
	return 1;
}
int shimFastOpen(int entry, unsigned int* length)
{
	if (!DVDFastOpen(entry, &sInfo)) {
		return 0;
	}
	*length = sInfo.length;
	return 1;
}
int shimRead(void* buf, int length, int offset) { return DVDReadPrio(&sInfo, buf, length, offset, 2); }
int shimReadAsync(void* buf, int length, int offset)
{
	sAsyncResult = -12345;
	DVDReadAsyncPrio(&sInfo, buf, length, offset, asyncDone, 2);
	return sAsyncResult; // (the stand-ins run the completion at once)
}
}
