// PS3 port: system backend (PSL1GHT / lv2). Compiled with PSL1GHT and newlib headers only.
// See ps3_backend.h for why this is separate from the SDK layer.

#include "ps3_backend.h"

#include <malloc.h>
#include <ppu-types.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ppu-lv2.h>
#include <lv2/syscalls.h>
#include <io/pad.h>
// File access uses the lv2 file-system system calls directly (sys/file.h), as PSL1GHT's own fopen does.
// The cellFs library (the sysFs* calls, module "sys_fs") is not loaded at start-up; calling it without
// sysModuleLoad(SYSMODULE_FS) jumps through an unresolved import. That crashed test build 1 at its first
// log line, before anything reached the screen or the USB drive.
#include <sys/file.h>
#include <sys/mutex.h>
#include <sys/cond.h>
#include <sys/process.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>

// Main thread: priority 1001, 1 MB stack (the game uses deep call chains and large stack frames).
SYS_PROCESS_PARAM(1001, 0x100000);

static volatile int sExitRequested = 0;

static void onSysutilEvent(u64 status, u64 param, void* userdata)
{
	(void)param;
	(void)userdata;
	if (status == SYSUTIL_EXIT_GAME) {
		sExitRequested = 1;
	}
}

extern "C" void ps3b_init(void)
{
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, onSysutilEvent, NULL);
}

extern "C" int ps3b_exit_requested(void)
{
	sysUtilCheckCallback();
	return sExitRequested;
}

extern "C" void ps3b_exit(int code)
{
	ps3b_log_write("[ps3] exiting to XMB\n");
	ps3b_log_flush();
	ps3b_audio_shutdown();
	ps3b_console_shutdown();
	sysProcessExit(code);
	for (;;) { }
}

extern "C" void* ps3b_memalign(unsigned int align, unsigned int size)
{
	return memalign(align, size);
}

extern "C" void ps3b_free(void* p)
{
	free(p);
}

// ---------------------------------------------------------------- time

extern "C" unsigned long long ps3b_timebase(void)
{
	u64 tb;
	__asm__ volatile("mftb %0" : "=r"(tb));
	return tb;
}

extern "C" unsigned long long ps3b_timebase_freq(void)
{
	return sysGetTimebaseFrequency();
}

extern "C" long long ps3b_epoch_seconds(void)
{
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return (long long)sec;
}

extern "C" void ps3b_sleep_us(unsigned int usec)
{
	sysUsleep(usec);
}

// ---------------------------------------------------------------- mutex / cond

extern "C" int ps3b_mutex_create(unsigned int* out)
{
	sys_mutex_attr_t attr;
	sysMutexAttrInitialize(attr);
	sys_mutex_t m;
	int r = sysMutexCreate(&m, &attr);
	*out = (unsigned int)m;
	return r;
}

extern "C" void ps3b_mutex_lock(unsigned int m)
{
	sysMutexLock((sys_mutex_t)m, 0);
}

extern "C" void ps3b_mutex_unlock(unsigned int m)
{
	sysMutexUnlock((sys_mutex_t)m);
}

extern "C" int ps3b_cond_create(unsigned int* out, unsigned int mutex)
{
	sys_cond_attr_t attr;
	sysCondAttrInitialize(attr);
	sys_cond_t c;
	int r   = sysCondCreate(&c, (sys_mutex_t)mutex, &attr);
	*out    = (unsigned int)c;
	return r;
}

extern "C" int ps3b_cond_wait(unsigned int c, unsigned int timeout_usec)
{
	return sysCondWait((sys_cond_t)c, timeout_usec) == 0 ? 0 : 1;
}

extern "C" void ps3b_cond_signal(unsigned int c)
{
	sysCondSignal((sys_cond_t)c);
}

extern "C" void ps3b_cond_broadcast(unsigned int c)
{
	sysCondBroadcast((sys_cond_t)c);
}

// ---------------------------------------------------------------- threads

extern "C" int ps3b_thread_create(void (*entry)(void*), void* arg, int priority, unsigned int stack_size, const char* name)
{
	sys_ppu_thread_t tid;
	char nameBuf[28];
	strncpy(nameBuf, name ? name : "thread", sizeof(nameBuf) - 1);
	nameBuf[sizeof(nameBuf) - 1] = 0;
	return sysThreadCreate(&tid, entry, arg, priority, stack_size, 0, nameBuf);
}

extern "C" void ps3b_thread_exit(void)
{
	sysThreadExit(0);
	for (;;) { }
}

// ---------------------------------------------------------------- files

extern "C" int ps3b_file_exists(const char* path)
{
	sysFSStat st;
	return sysLv2FsStat(path, &st) == 0;
}

extern "C" int ps3b_file_open(const char* path)
{
	s32 fd = -1;
	if (sysLv2FsOpen(path, SYS_O_RDONLY, &fd, 0, NULL, 0) != 0) {
		return -1;
	}
	return fd;
}

extern "C" long long ps3b_file_size(int fd)
{
	sysFSStat st;
	if (sysLv2FsFStat(fd, &st) != 0) {
		return -1;
	}
	return (long long)st.st_size;
}

extern "C" int ps3b_file_pread(int fd, void* buf, unsigned long long offset, unsigned int len)
{
	u64 pos = 0, got = 0;
	if (sysLv2FsLSeek64(fd, offset, 0 /* SEEK_SET */, &pos) != 0) {
		return -1;
	}
	if (sysLv2FsRead(fd, buf, len, &got) != 0) {
		return -1;
	}
	return (int)got;
}

extern "C" void ps3b_file_close(int fd)
{
	sysLv2FsClose(fd);
}

extern "C" int ps3b_file_open_rw(const char* path, int create)
{
	s32 fd = -1;
	if (sysLv2FsOpen(path, SYS_O_RDWR | (create ? SYS_O_CREAT : 0), &fd, 0666, NULL, 0) != 0) {
		return -1;
	}
	if (create) {
		sysLv2FsChmod(path, 0666);
	}
	return fd;
}

extern "C" int ps3b_file_pwrite(int fd, const void* buf, unsigned long long offset, unsigned int len)
{
	u64 pos = 0, written = 0;
	if (sysLv2FsLSeek64(fd, offset, 0 /* SEEK_SET */, &pos) != 0) {
		return -1;
	}
	if (sysLv2FsWrite(fd, buf, len, &written) != 0) {
		return -1;
	}
	return (int)written;
}

extern "C" int ps3b_file_read_all(const char* path, char** out_data, unsigned int* out_size)
{
	int fd = ps3b_file_open(path);
	if (fd < 0) {
		return 0;
	}
	long long size = ps3b_file_size(fd);
	if (size < 0) {
		ps3b_file_close(fd);
		return 0;
	}
	char* data = (char*)malloc((size_t)size + 1);
	int got    = data ? ps3b_file_pread(fd, data, 0, (unsigned int)size) : -1;
	ps3b_file_close(fd);
	if (got != size) {
		free(data);
		return 0;
	}
	data[size] = 0;
	*out_data  = data;
	*out_size  = (unsigned int)size;
	return 1;
}

// ---------------------------------------------------------------- data root + log file

static const char* sDataRootCandidates[] = {
	"/dev_usb000/PIKMIN", "/dev_usb001/PIKMIN", "/dev_usb002/PIKMIN", "/dev_usb003/PIKMIN",
	"/dev_usb004/PIKMIN", "/dev_usb005/PIKMIN", "/dev_usb006/PIKMIN", "/dev_hdd0/PIKMIN",
	"/dev_hdd0/game/PIKM00001/USRDIR/PIKMIN",
};
static const char* sDataRoot = NULL;
static int sDataRootSearched = 0;

extern "C" const char* ps3b_data_root(void)
{
	if (!sDataRootSearched) {
		sDataRootSearched = 1;
		char path[256];
		for (unsigned i = 0; i < sizeof(sDataRootCandidates) / sizeof(sDataRootCandidates[0]); i++) {
			snprintf(path, sizeof(path), "%s/fst.txt", sDataRootCandidates[i]);
			if (ps3b_file_exists(path)) {
				sDataRoot = sDataRootCandidates[i];
				break;
			}
		}
	}
	return sDataRoot;
}

// ---------------------------------------------------------------- log
//
// Log lines go to the on-screen console, the lv2 TTY and PIKMIN/pikmin_log.txt. Writing to the USB
// drive takes milliseconds per line (more while something else writes to the drive), and up to test
// build 14 it happened on the thread that logged, holding up every other game thread meanwhile. Lines
// logged while the sound was being mixed made the audio thread miss its 17.5 ms deadlines: in test 14,
// blocks of sound played twice, the demo movie's sound was choppy, and the boot "Pikmin!" voice was cut
// off after 20 ms. Now a line is only copied into a queue in memory; a background thread writes the
// queue out in batches, about ten times a second at most. ps3b_log_flush() writes what is queued right
// away (on exit).

static char sLogPath[256];
static int sLogState                = 0; // 0 = not chosen yet, 1 = writing, -1 = no location / gave up
static unsigned long long sLogBytes = 0;
static int sLogLimitNoted           = 0;
#define LOG_FILE_LIMIT (8ULL * 1024 * 1024)

enum { LOG_QUEUE = 256 * 1024, LOG_BATCH = 16 * 1024 };
static char sLogQueue[LOG_QUEUE];
static unsigned int sLogQueueHead, sLogQueueTail; // bytes taken / added (positions modulo LOG_QUEUE)
static unsigned int sLogDropped;                  // lines that did not fit in the queue
static unsigned int sLogQueueMutex, sLogQueueCond;
static unsigned int sLogOutMutex;   // held while writing out, so that lines leave in order
static int sLogThreadState = 0;     // 0 = not started, 1 = running, -1 = could not start (write directly)
static volatile int sLogInitDone = 0;

// The console keeps the most recent lines for the on-screen view (see ps3_console.cpp).
extern "C" void ps3b_console_append(const char* text);

static int createLogFile(const char* path)
{
	s32 fd = -1;
	if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, NULL, 0) != 0) {
		return 0;
	}
	sysLv2FsClose(fd);
	sysLv2FsChmod(path, 0666);
	snprintf(sLogPath, sizeof(sLogPath), "%s", path);
	return 1;
}

static void chooseLogFile()
{
	char path[256];
	const char* root = ps3b_data_root();
	if (root) {
		snprintf(path, sizeof(path), "%s/pikmin_log.txt", root);
		if (createLogFile(path)) {
			sLogState = 1;
			return;
		}
	}
	// No data folder: put the log at the top of the first USB drive that accepts it.
	for (int i = 0; i < 8; i++) {
		char dev[32];
		snprintf(dev, sizeof(dev), "/dev_usb00%d", i);
		if (!ps3b_file_exists(dev)) {
			continue;
		}
		snprintf(path, sizeof(path), "%s/pikmin_log.txt", dev);
		if (createLogFile(path)) {
			sLogState = 1;
			return;
		}
	}
	sLogState = -1;
}

// Open, append, close: nothing is lost if the console freezes and has to be switched off (the FAT
// directory entry is only updated on close).
static void appendLogFile(const char* text, unsigned int len)
{
	if (sLogState != 1 || len == 0) {
		return;
	}
	const char* note = "[ps3] log file limit reached; further lines only on screen\n";
	if (sLogBytes + len > LOG_FILE_LIMIT) {
		if (sLogLimitNoted) {
			return;
		}
		sLogLimitNoted = 1;
		text           = note;
		len            = strlen(note);
	}
	s32 fd = -1;
	if (sysLv2FsOpen(sLogPath, SYS_O_WRONLY, &fd, 0, NULL, 0) != 0) {
		return;
	}
	u64 pos = 0, written = 0;
	sysLv2FsLSeek64(fd, 0, 2 /* SEEK_END */, &pos);
	sysLv2FsWrite(fd, text, len, &written);
	sysLv2FsClose(fd);
	sLogBytes += len;
}

// Writes text (len bytes, NUL-terminated) to the TTY, the screen and the file. sLogOutMutex held.
static void emitLog(const char* text, unsigned int len)
{
	fwrite(text, 1, len, stdout); // lv2 TTY (visible in RPCS3's log and on debug consoles)
	ps3b_console_append(text);
	appendLogFile(text, len);
}

// Takes everything queued (in LOG_BATCH pieces) and writes it out. sLogOutMutex held.
static void drainLogQueue()
{
	static char batch[LOG_BATCH + 1];
	for (;;) {
		ps3b_mutex_lock(sLogQueueMutex);
		unsigned int n = sLogQueueTail - sLogQueueHead;
		if (n > LOG_BATCH) {
			n = LOG_BATCH;
		}
		for (unsigned int i = 0; i < n; i++) {
			batch[i] = sLogQueue[(sLogQueueHead + i) % LOG_QUEUE];
		}
		sLogQueueHead += n;
		unsigned int dropped = sLogDropped;
		sLogDropped          = 0;
		ps3b_mutex_unlock(sLogQueueMutex);
		if (n == 0 && dropped == 0) {
			return;
		}
		batch[n] = 0;
		emitLog(batch, n);
		if (dropped) {
			char note[96];
			int len = snprintf(note, sizeof(note), "[ps3] %u log lines lost (written faster than the USB drive takes them)\n", dropped);
			emitLog(note, (unsigned int)len);
		}
	}
}

static void logThread(void*)
{
	for (;;) {
		ps3b_mutex_lock(sLogQueueMutex);
		while (sLogQueueTail == sLogQueueHead && sLogDropped == 0) {
			ps3b_cond_wait(sLogQueueCond, 500 * 1000);
		}
		ps3b_mutex_unlock(sLogQueueMutex);
		ps3b_sleep_us(100 * 1000); // let a burst of lines collect: one file append for all of them
		ps3b_mutex_lock(sLogOutMutex);
		drainLogQueue();
		ps3b_mutex_unlock(sLogOutMutex);
	}
}

static void initLog()
{
	// The first line comes from the boot thread, before any other thread exists.
	ps3b_mutex_create(&sLogQueueMutex);
	ps3b_mutex_create(&sLogOutMutex);
	chooseLogFile();
	if (ps3b_cond_create(&sLogQueueCond, sLogQueueMutex) == 0
	    && ps3b_thread_create(logThread, NULL, 1400, 0x8000, "pikmin_log") == 0) {
		sLogThreadState = 1;
	} else {
		sLogThreadState = -1;
	}
	sLogInitDone = 1;
}

extern "C" void ps3b_log_write(const char* text)
{
	if (!sLogInitDone) {
		initLog();
	}
	unsigned int len = strlen(text);
	if (sLogThreadState != 1) {
		ps3b_mutex_lock(sLogOutMutex);
		emitLog(text, len);
		ps3b_mutex_unlock(sLogOutMutex);
		return;
	}
	ps3b_mutex_lock(sLogQueueMutex);
	if (len > LOG_QUEUE - (sLogQueueTail - sLogQueueHead)) {
		sLogDropped++;
	} else {
		for (unsigned int i = 0; i < len; i++) {
			sLogQueue[(sLogQueueTail + i) % LOG_QUEUE] = text[i];
		}
		sLogQueueTail += len;
	}
	ps3b_cond_signal(sLogQueueCond);
	ps3b_mutex_unlock(sLogQueueMutex);
}

extern "C" void ps3b_log_flush(void)
{
	if (!sLogInitDone) {
		return;
	}
	ps3b_mutex_lock(sLogOutMutex);
	drainLogQueue();
	ps3b_mutex_unlock(sLogOutMutex);
}

extern "C" const char* ps3b_log_path(void)
{
	return sLogState == 1 ? sLogPath : NULL;
}

static inline s32 sysMemoryGetUserMemorySizeRaw(u32* info /* [0] = total, [1] = available */)
{
	lv2syscall1(SYSCALL_MEMORY_GET_USER_MEMORY_SIZE, (u64)info);
	return_to_user_prog(s32);
}

extern "C" void ps3b_log_memory(const char* when)
{
	u32 info[2] = { 0, 0 };
	char buf[160];
	if (sysMemoryGetUserMemorySizeRaw(info) == 0) {
		snprintf(buf, sizeof(buf), "[ps3] memory (%s): %u KB free of %u KB\n", when, info[1] >> 10, info[0] >> 10);
	} else {
		snprintf(buf, sizeof(buf), "[ps3] memory (%s): unknown\n", when);
	}
	ps3b_log_write(buf);
}

// ---------------------------------------------------------------- controllers

static padData sPadLast[4];
static int sPadValid[4]; // a report has arrived for this port
static int sPadInitDone;
static unsigned int sPadMutex;

// Called early by the platform start-up (the console watches for the exit combination) and again by
// the game's PADInit; only the first call does anything.
extern "C" void ps3b_pad_init(void)
{
	if (sPadInitDone) {
		return;
	}
	sPadInitDone = 1;
	ps3b_mutex_create(&sPadMutex);
	memset(sPadLast, 0, sizeof(sPadLast));
	memset(sPadValid, 0, sizeof(sPadValid));
	ioPadInit(7);
}

extern "C" void ps3b_pad_read(int port, Ps3bPad* out)
{
	memset(out, 0, sizeof(*out));
	out->lx = out->ly = out->rx = out->ry = 128;
	if (port < 0 || port >= 4 || !sPadInitDone) {
		return;
	}
	// Both the game and the console thread read the pads; reads consume "changed" reports, so the last
	// report is cached and shared.
	ps3b_mutex_lock(sPadMutex);
	padInfo info;
	if (ioPadGetInfo(&info) != 0 || !info.status[port]) {
		ps3b_mutex_unlock(sPadMutex);
		return;
	}
	padData data;
	if (ioPadGetData(port, &data) == 0 && data.len > 0) {
		sPadLast[port]  = data; // len == 0 means "no change since last read"
		sPadValid[port] = 1;
	}
	const padData d   = sPadLast[port];
	const int valid   = sPadValid[port];
	ps3b_mutex_unlock(sPadMutex);
	out->connected   = 1;
	unsigned int b   = 0;
	if (d.BTN_LEFT) b |= PS3B_BTN_LEFT;
	if (d.BTN_DOWN) b |= PS3B_BTN_DOWN;
	if (d.BTN_RIGHT) b |= PS3B_BTN_RIGHT;
	if (d.BTN_UP) b |= PS3B_BTN_UP;
	if (d.BTN_START) b |= PS3B_BTN_START;
	if (d.BTN_R3) b |= PS3B_BTN_R3;
	if (d.BTN_L3) b |= PS3B_BTN_L3;
	if (d.BTN_SELECT) b |= PS3B_BTN_SELECT;
	if (d.BTN_SQUARE) b |= PS3B_BTN_SQUARE;
	if (d.BTN_CROSS) b |= PS3B_BTN_CROSS;
	if (d.BTN_CIRCLE) b |= PS3B_BTN_CIRCLE;
	if (d.BTN_TRIANGLE) b |= PS3B_BTN_TRIANGLE;
	if (d.BTN_R1) b |= PS3B_BTN_R1;
	if (d.BTN_L1) b |= PS3B_BTN_L1;
	if (d.BTN_R2) b |= PS3B_BTN_R2;
	if (d.BTN_L2) b |= PS3B_BTN_L2;
	out->buttons = b;
	if (valid) {
		out->lx = d.ANA_L_H;
		out->ly = d.ANA_L_V;
		out->rx = d.ANA_R_H;
		out->ry = d.ANA_R_V;
	}
	// Pressure values need pressure mode; use digital L2/R2 as full presses until then.
	out->l2 = d.BTN_L2 ? 255 : 0;
	out->r2 = d.BTN_R2 ? 255 : 0;
}
