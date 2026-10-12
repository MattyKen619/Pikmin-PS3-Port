// PS3 port: DVD access over the game files extracted by tools/ps3/extract_disc.py.
//
// Layout of the data folder (found by ps3b_data_root(), e.g. /dev_usb000/PIKMIN):
//   fst.txt   "<entrynum>\t<d|f>\t/<path>" for every disc file-table entry
//   files/    the files themselves
// Paths are matched case-insensitively, as the GameCube does.

#include "sdk_internal.h"

#include "../backend/ps3_backend.h"
#include "Dolphin/dvd.h"
#include <stdio.h>
#include <string.h>

#define HASH_SIZE   16384
#define MAX_OPEN_FD 24

struct DvdEntry {
	char* path;  // original case, leading '/'
	char* lower; // lower case, no leading '/'
	int isDir;
	int fd;
	long long size;
	u32 lastUse;
};

static DvdEntry* sEntries;
static int sEntryCount;
static int* sHash; // index+1 into sEntries, 0 = empty
static int sOpenCount;
static u32 sUseClock;
static int sLoaded;
static DVDDiskID sDiskId;

static void* allocZeroed(unsigned int size)
{
	void* p = ps3b_memalign(16, size ? size : 1);
	if (p) {
		memset(p, 0, size);
	}
	return p;
}

static int parseInt(const char* s)
{
	int v = 0;
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (*s++ - '0');
	}
	return v;
}

static u32 hashString(const char* s)
{
	u32 h = 2166136261u;
	for (; *s; s++) {
		h = (h ^ (u8)*s) * 16777619u;
	}
	return h;
}

static void normalize(const char* in, char* out, int outSize)
{
	int n = 0;
	while (*in == '/' || *in == '\\') {
		in++;
	}
	char prev = 0;
	for (; *in && n < outSize - 1; in++) {
		char c = *in;
		if (c == '\\') {
			c = '/';
		}
		if (c == '/' && prev == '/') {
			continue;
		}
		if (c >= 'A' && c <= 'Z') {
			c = (char)(c - 'A' + 'a');
		}
		out[n++] = c;
		prev     = c;
	}
	while (n > 0 && out[n - 1] == '/') {
		n--;
	}
	out[n] = 0;
}

extern "C" void ps3DvdInit(void)
{
	if (sLoaded) {
		return;
	}
	sLoaded          = 1;
	const char* root = ps3b_data_root();
	if (!root) {
		return;
	}
	char path[256];
	snprintf(path, sizeof(path), "%s/fst.txt", root);
	char* text = NULL;
	unsigned int size = 0;
	if (!ps3b_file_read_all(path, &text, &size)) {
		ps3Log("[ps3] FAILED to read %s\n", path);
		return;
	}
	int lines = 0;
	for (unsigned int i = 0; i < size; i++) {
		if (text[i] == '\n') {
			lines++;
		}
	}
	sEntries = (DvdEntry*)allocZeroed((lines + 1) * sizeof(DvdEntry));
	sHash    = (int*)allocZeroed(HASH_SIZE * sizeof(int));
	char* p  = text;
	while (*p) {
		char* eol = strchr(p, '\n');
		if (eol) {
			*eol = 0;
		}
		int num    = parseInt(p);
		char* tab1 = strchr(p, '\t');
		char* tab2 = tab1 ? strchr(tab1 + 1, '\t') : NULL;
		if (tab1 && tab2 && num == sEntryCount) {
			DvdEntry* e = &sEntries[sEntryCount];
			e->isDir    = tab1[1] == 'd';
			e->path     = tab2 + 1;
			char low[256];
			normalize(e->path, low, sizeof(low));
			e->lower = (char*)allocZeroed(strlen(low) + 1);
			strcpy(e->lower, low);
			e->fd   = -1;
			e->size = -1;
			u32 h   = hashString(e->lower) & (HASH_SIZE - 1);
			while (sHash[h]) {
				h = (h + 1) & (HASH_SIZE - 1);
			}
			sHash[h] = sEntryCount + 1;
			sEntryCount++;
		}
		if (!eol) {
			break;
		}
		p = eol + 1;
	}
	// text is kept: the entries point into it.
	memcpy(sDiskId.gameName, "GPIE", 4);
	memcpy(sDiskId.company, "01", 2);
	sDiskId.diskNumber  = 0;
	sDiskId.gameVersion = 1;
	ps3Log("[ps3] disc file table: %d entries\n", sEntryCount);
}

static int findEntry(const char* path)
{
	if (!sEntries) {
		return -1;
	}
	char low[256];
	normalize(path, low, sizeof(low));
	u32 h = hashString(low) & (HASH_SIZE - 1);
	while (sHash[h]) {
		int idx = sHash[h] - 1;
		if (strcmp(sEntries[idx].lower, low) == 0) {
			return idx;
		}
		h = (h + 1) & (HASH_SIZE - 1);
	}
	return -1;
}

static int entryFd(int idx)
{
	DvdEntry* e = &sEntries[idx];
	e->lastUse  = ++sUseClock;
	if (e->fd >= 0) {
		return e->fd;
	}
	if (sOpenCount >= MAX_OPEN_FD) {
		int oldest = -1;
		for (int i = 0; i < sEntryCount; i++) {
			if (sEntries[i].fd >= 0 && (oldest < 0 || sEntries[i].lastUse < sEntries[oldest].lastUse)) {
				oldest = i;
			}
		}
		if (oldest >= 0) {
			ps3b_file_close(sEntries[oldest].fd);
			sEntries[oldest].fd = -1;
			sOpenCount--;
		}
	}
	char full[512];
	snprintf(full, sizeof(full), "%s/files%s", ps3b_data_root(), e->path);
	e->fd = ps3b_file_open(full);
	if (e->fd < 0) {
		ps3Log("[ps3] FAILED to open %s\n", full);
		return -1;
	}
	sOpenCount++;
	if (e->size < 0) {
		e->size = ps3b_file_size(e->fd);
	}
	return e->fd;
}

static BOOL fillInfo(int idx, DVDFileInfo* info)
{
	if (idx < 0 || idx >= sEntryCount || sEntries[idx].isDir) {
		return FALSE;
	}
	if (sEntries[idx].size < 0 && entryFd(idx) < 0) {
		return FALSE;
	}
	memset(info, 0, sizeof(*info));
	info->startAddr     = (u32)idx; // our convention: the entry number identifies the file
	info->length        = (u32)sEntries[idx].size;
	info->cBlock.state  = DVD_STATE_END;
	return TRUE;
}

void DVDInit(void)
{
	ps3NoteActivity("DVDInit");
	ps3DvdInit();
}

s32 DVDConvertPathToEntrynum(const char* path)
{
	return findEntry(path);
}

BOOL DVDOpen(const char* path, DVDFileInfo* info)
{
	ps3NoteActivity("DVDOpen");
	int idx = findEntry(path);
	if (!fillInfo(idx, info)) {
		ps3Log("[ps3] DVDOpen: not found: %s\n", path);
		return FALSE;
	}
	return TRUE;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* info)
{
	ps3NoteActivity("DVDFastOpen");
	return fillInfo(entrynum, info);
}

BOOL DVDClose(DVDFileInfo* info)
{
	(void)info;
	return TRUE;
}

static s32 doRead(DVDFileInfo* info, void* addr, s32 length, s32 offset)
{
	int idx = (int)info->startAddr;
	if (idx < 0 || idx >= sEntryCount || length < 0) {
		return DVD_RESULT_FATAL_ERROR;
	}
	int fd = entryFd(idx);
	if (fd < 0) {
		return DVD_RESULT_FATAL_ERROR;
	}
	int got = ps3b_file_pread(fd, addr, (unsigned long long)offset, (unsigned int)length);
	if (got < 0) {
		ps3Log("[ps3] FAILED reading %s (offset %d, %d bytes)\n", sEntries[idx].path, (int)offset, (int)length);
		return DVD_RESULT_FATAL_ERROR;
	}
	if (got < length) {
		// The GameCube reads in 32-byte units past the end of a file; callers expect the full length.
		memset((u8*)addr + got, 0, length - got);
	}
	return length;
}

s32 DVDReadPrio(DVDFileInfo* info, void* addr, s32 length, s32 offset, s32 prio)
{
	(void)prio;
	ps3NoteActivity("DVDReadPrio");
	ps3DeliverInterrupts();
	return doRead(info, addr, length, offset);
}

static void asyncDone(void* arg)
{
	DVDFileInfo* info  = (DVDFileInfo*)arg;
	s32 result         = (s32)info->cBlock.transferredSize;
	info->cBlock.state = (result < 0) ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
	if (info->callback) {
		info->callback(result, info);
	}
}

// The data is read straight away; the callback runs at the next scheduling point, like the
// GameCube's DVD interrupt would.
BOOL DVDReadAsyncPrio(DVDFileInfo* info, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio)
{
	(void)prio;
	ps3NoteActivity("DVDReadAsyncPrio");
	info->callback               = callback;
	info->cBlock.state           = DVD_STATE_BUSY;
	info->cBlock.transferredSize = (u32)doRead(info, addr, length, offset);
	ps3PostInterrupt(asyncDone, info);
	return TRUE;
}

s32 DVDGetDriveStatus(void)
{
	ps3DeliverInterrupts();
	return DVD_STATE_END;
}

DVDDiskID* DVDGetCurrentDiskID(void)
{
	return &sDiskId;
}

// Drive audio streaming (DTK) is part of the audio work (Phase 5).
BOOL DVDPrepareStreamAsync(DVDFileInfo* info, u32 length, u32 offset, DVDCallback callback)
{
	(void)info;
	(void)length;
	(void)offset;
	(void)callback;
	static int sLogged;
	ps3LogOnce(&sLogged, "[ps3] disc audio streaming: not implemented yet\n");
	return FALSE;
}

BOOL DVDCancelStream(DVDCommandBlock* block)
{
	(void)block;
	return TRUE;
}
