// PS3 port: system backend (PSL1GHT / lv2). Compiled with PSL1GHT and newlib headers only.
// See ps3_backend.h for why this is separate from the SDK layer.

#include "ps3_backend.h"

#include <malloc.h>
#include <ppu-types.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/syscalls.h>
#include <io/pad.h>
#include <lv2/sysfs.h>
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
	sysProcessExit(code);
}

extern "C" void* ps3b_memalign(unsigned int align, unsigned int size)
{
	return memalign(align, size);
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
	return sysFsStat(path, &st) == 0;
}

extern "C" int ps3b_file_open(const char* path)
{
	s32 fd = -1;
	if (sysFsOpen(path, SYS_O_RDONLY, &fd, NULL, 0) != 0) {
		return -1;
	}
	return fd;
}

extern "C" long long ps3b_file_size(int fd)
{
	sysFSStat st;
	if (sysFsFstat(fd, &st) != 0) {
		return -1;
	}
	return (long long)st.st_size;
}

extern "C" int ps3b_file_pread(int fd, void* buf, unsigned long long offset, unsigned int len)
{
	u64 pos = 0, got = 0;
	if (sysFsLseek(fd, (s64)offset, 0 /* SEEK_SET */, &pos) != 0) {
		return -1;
	}
	if (sysFsRead(fd, buf, len, &got) != 0) {
		return -1;
	}
	return (int)got;
}

extern "C" void ps3b_file_close(int fd)
{
	sysFsClose(fd);
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

static unsigned int sLogMutex = 0;
static int sLogFd             = -1;
static int sLogFileTried      = 0;

// The console keeps the most recent lines for the on-screen view (see ps3_console.cpp).
extern "C" void ps3b_console_append(const char* text);

extern "C" void ps3b_log_write(const char* text)
{
	if (!sLogMutex) {
		ps3b_mutex_create(&sLogMutex);
	}
	ps3b_mutex_lock(sLogMutex);
	fputs(text, stdout); // lv2 TTY (visible in RPCS3's log and on debug consoles)
	if (!sLogFileTried) {
		const char* root = ps3b_data_root();
		if (root) {
			sLogFileTried = 1;
			char path[256];
			snprintf(path, sizeof(path), "%s/pikmin_log.txt", root);
			s32 fd = -1;
			if (sysFsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, NULL, 0) == 0) {
				sLogFd = fd;
			}
		}
	}
	if (sLogFd >= 0) {
		u64 written = 0;
		sysFsWrite(sLogFd, text, strlen(text), &written);
	}
	ps3b_console_append(text);
	ps3b_mutex_unlock(sLogMutex);
}

// ---------------------------------------------------------------- controllers

static padData sPadLast[4];
static int sPadValid[4]; // a report has arrived for this port

extern "C" void ps3b_pad_init(void)
{
	ioPadInit(7);
	memset(sPadLast, 0, sizeof(sPadLast));
	memset(sPadValid, 0, sizeof(sPadValid));
}

extern "C" void ps3b_pad_read(int port, Ps3bPad* out)
{
	memset(out, 0, sizeof(*out));
	out->lx = out->ly = out->rx = out->ry = 128;
	if (port < 0 || port >= 4) {
		return;
	}
	padInfo info;
	if (ioPadGetInfo(&info) != 0 || !info.status[port]) {
		return;
	}
	padData data;
	if (ioPadGetData(port, &data) == 0 && data.len > 0) {
		sPadLast[port]  = data; // len == 0 means "no change since last read"
		sPadValid[port] = 1;
	}
	const padData& d = sPadLast[port];
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
	if (sPadValid[port]) {
		out->lx = d.ANA_L_H;
		out->ly = d.ANA_L_V;
		out->rx = d.ANA_R_H;
		out->ry = d.ANA_R_V;
	}
	// Pressure values need pressure mode; use digital L2/R2 as full presses until then.
	out->l2 = d.BTN_L2 ? 255 : 0;
	out->r2 = d.BTN_R2 ? 255 : 0;
}
