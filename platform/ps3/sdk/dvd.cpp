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
	int busy; // reads queued on the I/O thread: the file must stay open
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

	// Resolve "." and ".." components, as the GameCube's DVD path lookup does
	// (e.g. "/dataDir/SndData/../MovieData/cntA_S.h4m").
	if (strstr(out, "./") || strcmp(out, ".") == 0 || (n >= 2 && strcmp(out + n - 2, "/.") == 0)
	    || (n >= 3 && strcmp(out + n - 3, "/..") == 0) || strcmp(out, "..") == 0) {
		char tmp[256];
		int t = 0;
		const char* q = out;
		while (*q) {
			const char* e = strchr(q, '/');
			int len       = e ? (int)(e - q) : (int)strlen(q);
			if (len == 1 && q[0] == '.') {
				// stay
			} else if (len == 2 && q[0] == '.' && q[1] == '.') {
				while (t > 0 && tmp[t - 1] != '/') {
					t--;
				}
				if (t > 0) {
					t--; // drop the slash
				}
			} else if (len > 0 && t + len + 1 < (int)sizeof(tmp)) {
				if (t > 0) {
					tmp[t++] = '/';
				}
				memcpy(tmp + t, q, len);
				t += len;
			}
			q += len;
			if (*q == '/') {
				q++;
			}
		}
		tmp[t] = 0;
		strncpy(out, tmp, outSize - 1);
		out[outSize - 1] = 0;
	}
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
			if (sEntries[i].fd >= 0 && !sEntries[i].busy && (oldest < 0 || sEntries[i].lastUse < sEntries[oldest].lastUse)) {
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
	PS3_ACTIVITY("DVDInit");
	ps3DvdInit();
}

s32 DVDConvertPathToEntrynum(const char* path)
{
	return findEntry(path);
}

BOOL DVDOpen(const char* path, DVDFileInfo* info)
{
	PS3_ACTIVITY("DVDOpen");
	int idx = findEntry(path);
	if (!fillInfo(idx, info)) {
		ps3Log("[ps3] DVDOpen: not found: %s\n", path);
		return FALSE;
	}
	ps3Log("[dvd] open %s (%u bytes)\n", path, (unsigned)info->length); // progress trail for test builds
	return TRUE;
}

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* info)
{
	PS3_ACTIVITY("DVDFastOpen");
	if (!fillInfo(entrynum, info)) {
		ps3Log("[ps3] DVDFastOpen: bad entry %d\n", (int)entrynum);
		return FALSE;
	}
	ps3Log("[dvd] open #%d %s (%u bytes)\n", (int)entrynum, sEntries[entrynum].path, (unsigned)info->length);
	return TRUE;
}

BOOL DVDClose(DVDFileInfo* info)
{
	(void)info;
	return TRUE;
}

// ---- reading
//
// On the GameCube a DVD read is a DMA: the thread that asked waits (or carries on, for an async read)
// while the other threads run. Reading here, on the thread that holds the scheduler's baton, stopped every
// other game thread for the length of the read. While the game loaded its data, the audio thread could
// not mix, and the sound stuttered (test 13). Reads now go to an I/O thread outside the scheduler: a
// synchronous read gives up the baton until its data is in; an asynchronous one returns at once and its
// callback runs as an interrupt when the data is in, as on the GameCube.

struct IoReq {
	int entry;
	int fd;
	void* addr;
	u32 length;
	unsigned long long offset;
	s32 result;
	volatile int done;
	DVDFileInfo* asyncInfo; // async request: finish with asyncDone(asyncInfo)
	IoReq* next;
};

enum { IO_POOL = 32 };
static unsigned int sIoLock, sIoCv;
static IoReq* volatile sIoHead;
static IoReq* sIoTail;
static IoReq sIoPool[IO_POOL]; // async requests (game context only: the baton serialises access)
static IoReq* sIoFree;
static int sIoState; // 0 not started, 1 running, -1 unavailable

extern "C" int ps3CanBlock(void); // os_sched.cpp: a game thread holds the baton, not in an interrupt

static void asyncDone(void* arg)
{
	DVDFileInfo* info  = (DVDFileInfo*)arg;
	s32 result         = (s32)info->cBlock.transferredSize;
	info->cBlock.state = (result < 0) ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
	if (info->callback) {
		info->callback(result, info);
	}
}

// The read itself. Runs on the I/O thread (or, as a fallback, on the caller); touches no shared state.
static s32 readNow(int idx, int fd, void* addr, u32 length, unsigned long long offset)
{
	int got = ps3b_file_pread(fd, addr, offset, length);
	if (got < 0) {
		ps3Log("[ps3] FAILED reading %s (offset %d, %d bytes)\n", sEntries[idx].path, (int)offset, (int)length);
		return DVD_RESULT_FATAL_ERROR;
	}
	if ((u32)got < length) {
		// The GameCube reads in 32-byte units past the end of a file; callers expect the full length.
		memset((u8*)addr + got, 0, length - got);
	}
	return (s32)length;
}

static void ioComplete(void* arg);

static void ioThread(void*)
{
	for (;;) {
		ps3b_mutex_lock(sIoLock);
		while (!sIoHead) {
			ps3b_cond_wait(sIoCv, 100 * 1000);
		}
		IoReq* r = sIoHead;
		sIoHead  = r->next;
		if (!sIoHead) {
			sIoTail = NULL;
		}
		ps3b_mutex_unlock(sIoLock);
		r->result = readNow(r->entry, r->fd, r->addr, r->length, r->offset);
		ps3PostInterrupt(ioComplete, r);
	}
}

// Interrupt: a read finished.
static void ioComplete(void* arg)
{
	IoReq* r = (IoReq*)arg;
	sEntries[r->entry].busy--;
	if (r->asyncInfo) {
		DVDFileInfo* info            = r->asyncInfo;
		info->cBlock.transferredSize = (u32)r->result;
		r->next                      = sIoFree;
		sIoFree                      = r;
		asyncDone(info);
	} else {
		r->done = 1;
		ps3WakeAll(r);
	}
}

static bool ioAvailable()
{
	if (sIoState == 0) {
		sIoState = -1;
		if (ps3b_mutex_create(&sIoLock) == 0 && ps3b_cond_create(&sIoCv, sIoLock) == 0) {
			for (int i = 0; i < IO_POOL; i++) {
				sIoPool[i].next = sIoFree;
				sIoFree         = &sIoPool[i];
			}
			if (ps3b_thread_create(ioThread, NULL, 950, 0x8000, "pikmin_dvd_io") == 0) {
				sIoState = 1;
				ps3Log("[ps3] DVD reads run in the background\n");
			}
		}
		if (sIoState < 0) {
			ps3Log("[ps3] FAILED to start the DVD read thread; reading directly\n");
		}
	}
	return sIoState > 0;
}

static void queueIo(IoReq* r)
{
	sEntries[r->entry].busy++;
	r->next = NULL;
	ps3b_mutex_lock(sIoLock);
	if (sIoTail) {
		sIoTail->next = r;
	} else {
		sIoHead = r;
	}
	sIoTail = r;
	ps3b_cond_signal(sIoCv);
	ps3b_mutex_unlock(sIoLock);
}

// Checks a read and opens its file. Returns the file's fd, or -1.
static int prepareRead(DVDFileInfo* info, s32 length, int* idxOut)
{
	int idx = (int)info->startAddr;
	if (idx < 0 || idx >= sEntryCount || length < 0) {
		return -1;
	}
	*idxOut = idx;
	return entryFd(idx);
}

s32 DVDReadPrio(DVDFileInfo* info, void* addr, s32 length, s32 offset, s32 prio)
{
	(void)prio;
	PS3_ACTIVITY("DVDReadPrio");
	ps3DeliverInterrupts();
	int idx = 0;
	int fd  = prepareRead(info, length, &idx);
	if (fd < 0) {
		return DVD_RESULT_FATAL_ERROR;
	}
	if (!ps3CanBlock() || !ioAvailable()) {
		return readNow(idx, fd, addr, (u32)length, (unsigned long long)offset);
	}
	IoReq r;
	memset(&r, 0, sizeof(r));
	r.entry  = idx;
	r.fd     = fd;
	r.addr   = addr;
	r.length = (u32)length;
	r.offset = (unsigned long long)offset;
	queueIo(&r);
	while (!r.done) {
		ps3WaitOn(&r, "DVD read");
	}
	return r.result;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* info, void* addr, s32 length, s32 offset, DVDCallback callback, s32 prio)
{
	(void)prio;
	PS3_ACTIVITY("DVDReadAsyncPrio");
	info->callback     = callback;
	info->cBlock.state = DVD_STATE_BUSY;
	int idx            = 0;
	int fd             = prepareRead(info, length, &idx);
	if (fd >= 0 && ioAvailable() && sIoFree) {
		IoReq* r = sIoFree;
		sIoFree  = r->next;
		memset(r, 0, sizeof(*r));
		r->entry     = idx;
		r->fd        = fd;
		r->addr      = addr;
		r->length    = (u32)length;
		r->offset    = (unsigned long long)offset;
		r->asyncInfo = info;
		queueIo(r);
		return TRUE;
	}
	// No I/O thread or no free request: read now; the callback still runs as an interrupt.
	info->cBlock.transferredSize = (u32)(fd < 0 ? DVD_RESULT_FATAL_ERROR : readNow(idx, fd, addr, (u32)length, (unsigned long long)offset));
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
