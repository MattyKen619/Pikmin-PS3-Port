// PS3 port: system backend (PSL1GHT / lv2). Compiled with PSL1GHT and newlib headers only.
// See ps3_backend.h for why this is separate from the SDK layer.

#include "ps3_backend.h"
#include "../include/ps3/banner_icon.h"
#include "../include/ps3/disc_image.h"

#include <malloc.h>
#include <ppu-types.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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

static void (*volatile sExitHook)(void);

extern "C" void ps3b_set_exit_hook(void (*hook)(void)) { sExitHook = hook; }

extern "C" void ps3b_exit(int code)
{
	void (*hook)(void) = sExitHook;
	sExitHook          = 0;
	if (hook) {
		hook();
	}
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

extern "C" void ps3b_thread_stack(unsigned long long* lo, unsigned long long* hi)
{
	sys_ppu_thread_stack_t info;
	if (sysThreadGetStackInformation(&info) == 0) {
		*lo = (unsigned long long)(uintptr_t)info.addr;
		*hi = *lo + info.size;
	} else {
		*lo = *hi = 0;
	}
}

extern "C" int ps3b_address_readable(unsigned long long addr)
{
	// sys_memory_get_page_attribute (351): fails for an address that is not mapped
	u64 attr[4] __attribute__((aligned(16))); // (sys_page_attr_t: attribute, access_right, page_size + pad)
	lv2syscall2(351, addr, (u64)(uintptr_t)attr);
	return (s32)p1 == 0;
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

// ---------------------------------------------------------------- where things are (build 52)
//
// As other PS3 homebrew ports do it (Doom 64's, for one): the package holds only the program; the player puts
// the game's own data in the game's folder on the hard drive, /dev_hdd0/game/<title ID>/USRDIR, where it stays
// when a newer package is installed over this one. Here that is the Pikmin (North America) disc image, under
// any name ending in .iso, .gcm or .ciso; the game reads its files straight out of it (ps3/disc_image.h, dvd.cpp).
// For a player without a file manager or FTP: an image on a USB drive (at the top, or in a PIKMIN folder) is
// copied to the hard drive on the first start. The saves go to /dev_hdd0/data/pikmin, which survives deleting
// the game. The log goes to the game's folder, with a copy in the USB drive's PIKMIN folder if there is one.
// Older setups still work: the files extracted by tools/ps3/extract_disc.py, in a PIKMIN folder (fst.txt and
// files/) on a USB drive, at /dev_hdd0/PIKMIN, or in the game's folder.

#ifndef PS3_APPID
#define PS3_APPID "PIKM00001"
#endif
#define GAME_DIR      "/dev_hdd0/game/" PS3_APPID
#define GAME_USRDIR   GAME_DIR "/USRDIR"
#define SAVE_PARENT   "/dev_hdd0/data"
#define SAVE_DIR      SAVE_PARENT "/pikmin"
#define COPY_PARTIAL  GAME_USRDIR "/pikmin_copy.part" // an image being copied in (removed if the copy is cut short)

static int isDirectory(const char* path)
{
	sysFSStat st;
	return sysLv2FsStat(path, &st) == 0 && (st.st_mode & 0170000) == 0040000;
}

// (None of these log: the log file's location depends on them.)
extern "C" const char* ps3b_usrdir(void)
{
	static int sState; // 0 = not looked yet, 1 = there, -1 = not (not installed as a package)
	if (!sState) {
		sState = isDirectory(GAME_USRDIR) ? 1 : -1;
	}
	return sState > 0 ? GAME_USRDIR : NULL;
}

extern "C" const char* ps3b_usb_folder(void)
{
	static int sSearched;
	static char sPath[32];
	if (!sSearched) {
		sSearched = 1;
		for (int i = 0; i < 8 && !sPath[0]; i++) {
			char path[32];
			snprintf(path, sizeof(path), "/dev_usb00%d/PIKMIN", i);
			if (isDirectory(path)) {
				snprintf(sPath, sizeof(sPath), "%s", path);
			}
		}
	}
	return sPath[0] ? sPath : NULL;
}

extern "C" const char* ps3b_work_dir(void)
{
	if (const char* usb = ps3b_usb_folder()) {
		return usb;
	}
	if (const char* usrdir = ps3b_usrdir()) {
		return usrdir;
	}
	return ps3b_data_root();
}

static const char* sDataRootCandidates[] = {
	GAME_USRDIR "/PIKMIN", "/dev_usb000/PIKMIN", "/dev_usb001/PIKMIN", "/dev_usb002/PIKMIN", "/dev_usb003/PIKMIN",
	"/dev_usb004/PIKMIN", "/dev_usb005/PIKMIN", "/dev_usb006/PIKMIN", "/dev_usb007/PIKMIN", "/dev_hdd0/PIKMIN",
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

extern "C" const char* ps3b_save_dir(void)
{
	static int sState; // 0 = not set up yet, 1 = SAVE_DIR, 2 = the work folder, -1 = nowhere
	if (!sState) {
		sysLv2FsMkdir(SAVE_PARENT, 0777); // (fail harmlessly if they exist)
		sysLv2FsMkdir(SAVE_DIR, 0777);
		sState = isDirectory(SAVE_DIR) ? 1 : (ps3b_work_dir() ? 2 : -1);
	}
	return sState == 1 ? SAVE_DIR : sState == 2 ? ps3b_work_dir() : NULL;
}

static void bootLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void bootLog(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ps3b_log_write(buf);
}

static int sizeInMb(unsigned long long bytes) { return (int)((bytes + (1 << 19)) >> 20); }

// Copies a file, to `partial` first (so that a copy cut short never looks finished). With `progress`, logs how
// far it has got (the log is on screen during start-up) about every 5%.
static int copyFile(const char* from, const char* to, const char* partial, const char* progress)
{
	s32 in = -1, out = -1;
	if (sysLv2FsOpen(from, SYS_O_RDONLY, &in, 0, NULL, 0) != 0) {
		bootLog("[ps3] could not open %s\n", from);
		return 0;
	}
	const long long size = ps3b_file_size(in);
	sysLv2FsUnlink(partial);
	if (size < 0 || sysLv2FsOpen(partial, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &out, 0666, NULL, 0) != 0) {
		bootLog("[ps3] could not create %s\n", partial);
		sysLv2FsClose(in);
		return 0;
	}
	enum { CHUNK = 2 << 20 };
	char* buf = (char*)memalign(128, CHUNK);
	unsigned long long done = 0;
	const unsigned long long start = ps3b_timebase();
	int nextStep = 5, ok = buf != NULL;
	while (ok && done < (unsigned long long)size) {
		const unsigned long long left = (unsigned long long)size - done;
		const unsigned int want = left < CHUNK ? (unsigned int)left : (unsigned int)CHUNK;
		u64 got = 0, written = 0;
		if (sysLv2FsRead(in, buf, want, &got) != 0 || got == 0) {
			bootLog("[ps3] FAILED reading %s after %d MB\n", from, sizeInMb(done));
			ok = 0;
			break;
		}
		if (sysLv2FsWrite(out, buf, got, &written) != 0 || written != got) {
			bootLog("[ps3] FAILED writing %s after %d MB (is the hard drive full?)\n", partial, sizeInMb(done));
			ok = 0;
			break;
		}
		done += got;
		if (ps3b_exit_requested()) {
			bootLog("[ps3] copy stopped: the game is quitting\n");
			ok = 0;
			break;
		}
		const int percent = (int)(done * 100 / (unsigned long long)size);
		if (progress && percent >= nextStep) {
			const unsigned long long secs = (ps3b_timebase() - start) / ps3b_timebase_freq();
			bootLog("[boot] %s: %d%% (%d of %d MB, %d s)\n", progress, percent, sizeInMb(done), sizeInMb(size), (int)secs);
			nextStep = percent - percent % 5 + 5;
		}
	}
	free(buf);
	if (ok) {
		sysLv2FsFsync(out); // (on the disk before it gets its real name; not every file system has to support it)
	}
	sysLv2FsClose(out);
	sysLv2FsClose(in);
	if (ok) {
		sysLv2FsUnlink(to); // (only ever a damaged copy of the same file: see findImageIn)
		ok = sysLv2FsRename(partial, to) == 0;
		if (!ok) {
			bootLog("[ps3] FAILED to rename %s to %s\n", partial, to);
		}
	}
	if (!ok) {
		sysLv2FsUnlink(partial);
		return 0;
	}
	sysLv2FsChmod(to, 0666);
	return 1;
}

extern "C" int ps3b_file_copy(const char* from, const char* to)
{
	char partial[300];
	snprintf(partial, sizeof(partial), "%s.part", to);
	return copyFile(from, to, partial, NULL);
}

// ---- the disc image

static char sDiscImage[300];

extern "C" const char* ps3b_disc_image(void)
{
	return sDiscImage[0] ? sDiscImage : NULL;
}

static void* sCheckAllocs[4];
static int sCheckAllocCount;

static void* checkAlloc(unsigned int size)
{
	if (sCheckAllocCount >= 4) {
		return NULL;
	}
	void* p = malloc(size);
	sCheckAllocs[sCheckAllocCount++] = p;
	return p;
}

static int checkRead(void* ctx, void* dst, unsigned long long offset, unsigned int len)
{
	return ps3b_file_pread(*(int*)ctx, dst, offset, len);
}

// Why `path` is not a usable disc image, or NULL. Fills in the game ID and revision.
static const char* checkImage(const char* path, char id[7], int* revision)
{
	int fd = ps3b_file_open(path);
	if (fd < 0) {
		return "it cannot be opened";
	}
	discimg::Image image;
	const char* why = discimg::open(&image, checkRead, &fd, checkAlloc);
	if (!why && !discimg::gameId(&image, id, revision)) {
		why = "it cannot be read";
	}
	if (!why) {
		unsigned char* fst = NULL;
		if (!discimg::loadFst(&image, checkAlloc, &fst)) {
			why = "its file table cannot be read (a damaged or incomplete image?)";
		}
	}
	ps3b_file_close(fd);
	for (int i = 0; i < sCheckAllocCount; i++) {
		free(sCheckAllocs[i]);
	}
	sCheckAllocCount = 0;
	return why;
}

static int hasImageExtension(const char* name)
{
	const char* dot = strrchr(name, '.');
	if (!dot) {
		return 0;
	}
	char ext[8];
	int n = 0;
	for (const char* p = dot + 1; *p && n < 7; p++) {
		ext[n++] = (*p >= 'A' && *p <= 'Z') ? (char)(*p - 'A' + 'a') : *p;
	}
	ext[n] = 0;
	return !strcmp(ext, "iso") || !strcmp(ext, "gcm") || !strcmp(ext, "ciso");
}

// Looks in `dir` for the Pikmin (North America) disc image. 1 and its path (and file name) if found.
static int findImageIn(const char* dir, char* out, unsigned int outSize, const char** name)
{
	s32 dfd = -1;
	if (sysLv2FsOpenDir(dir, &dfd) != 0) {
		return 0;
	}
	int found = 0;
	static sysFSDirent ent;
	u64 n = 0;
	while (!found && sysLv2FsReadDir(dfd, &ent, &n) == 0 && n > 0) {
		if (!hasImageExtension(ent.d_name) || strncmp(ent.d_name, "._", 2) == 0) { // ("._" files: a Mac's notes)
			continue;
		}
		char path[300];
		snprintf(path, sizeof(path), "%s/%s", dir, ent.d_name);
		char id[7]   = "";
		int revision = 0;
		const char* why = checkImage(path, id, &revision);
		if (!why && strcmp(id, "GPIE01") != 0) {
			bootLog("[boot] not using %s: its game ID is %s; this port needs Pikmin for North America (GPIE01)\n", path, id);
			continue;
		}
		if (why) {
			bootLog("[boot] not using %s: %s\n", path, why);
			continue;
		}
		if (revision != 1) {
			bootLog("[boot] note: %s is revision %d of the disc; this port was made from revision 1\n", path, revision);
		}
		snprintf(out, outSize, "%s", path);
		if (name) {
			*name = strrchr(out, '/') + 1;
		}
		found = 1;
	}
	sysLv2FsCloseDir(dfd);
	return found;
}

static int findImageOnUsb(char* out, unsigned int outSize, const char** name)
{
	for (int i = 0; i < 8; i++) {
		char dir[32];
		snprintf(dir, sizeof(dir), "/dev_usb00%d", i);
		if (!isDirectory(dir)) {
			continue;
		}
		if (findImageIn(dir, out, outSize, name)) {
			return 1;
		}
		snprintf(dir, sizeof(dir), "/dev_usb00%d/PIKMIN", i);
		if (findImageIn(dir, out, outSize, name)) {
			return 1;
		}
	}
	return 0;
}

// Finds the game's data (see "where things are" above), copying a disc image from a USB drive to the hard drive
// on the first start. 1 if there is something to play from.
extern "C" int ps3b_prepare_game_data(void)
{
	const char* usrdir = ps3b_usrdir();
	if (usrdir) {
		sysLv2FsUnlink(COPY_PARTIAL); // a copy cut short last time (it starts again below)
		if (findImageIn(usrdir, sDiscImage, sizeof(sDiscImage), NULL)) {
			bootLog("[boot] game data: the disc image %s\n", sDiscImage);
			return 1;
		}
	}
	const char* root = ps3b_data_root();
	if (root && usrdir && strncmp(root, usrdir, strlen(usrdir)) == 0) {
		bootLog("[boot] game data: the extracted files in %s\n", root);
		return 1;
	}
	char usb[300];
	const char* name = NULL;
	if (findImageOnUsb(usb, sizeof(usb), &name)) {
		if (usrdir) {
			char to[300];
			snprintf(to, sizeof(to), "%s/%s", usrdir, name);
			bootLog("[boot] First start: copying the disc image %s to the hard drive (%s).\n", usb, to);
			bootLog("[boot] This happens once and takes a minute or two. The USB drive is not needed after that.\n");
			if (copyFile(usb, to, COPY_PARTIAL, "copying the disc image")) {
				bootLog("[boot] copied. From now on the game plays from the hard drive.\n");
				snprintf(sDiscImage, sizeof(sDiscImage), "%s", to);
				bootLog("[boot] game data: the disc image %s\n", sDiscImage);
				return 1;
			}
			bootLog("[boot] The copy did not work; playing from the USB drive this time.\n");
		}
		snprintf(sDiscImage, sizeof(sDiscImage), "%s", usb);
		bootLog("[boot] game data: the disc image %s\n", sDiscImage);
		return 1;
	}
	if (root) {
		bootLog("[boot] game data: the extracted files in %s\n", root);
		bootLog("[boot] (to play from the hard drive, put the Pikmin disc image in %s, or on the USB drive for the game to copy)\n",
		     usrdir ? usrdir : GAME_USRDIR);
		return 1;
	}
	return 0;
}

// ---- the XMB icon from the disc's banner (build 54; see ps3/banner_icon.h)

struct BannerFind {
	unsigned offset, length;
	int found;
};

static void findBanner(void* user, unsigned int, bool isDir, const char* path, unsigned int offset, unsigned int length)
{
	BannerFind* f = (BannerFind*)user;
	if (!isDir && !f->found && strcasecmp(path, "/opening.bnr") == 0) {
		f->offset = offset;
		f->length = length;
		f->found  = 1;
	}
}

// Reads the banner's picture (BANNER_BYTES) from the disc image or the extracted files. 1 if done.
static int readBanner(unsigned char* out)
{
	unsigned char bnr[bannericon::BANNER_OFFSET + bannericon::BANNER_BYTES];
	int ok = 0;
	if (sDiscImage[0]) {
		int fd = ps3b_file_open(sDiscImage);
		discimg::Image image;
		unsigned char* fst = NULL;
		unsigned int size  = 0;
		BannerFind find    = { 0, 0, 0 };
		if (fd >= 0 && !discimg::open(&image, checkRead, &fd, checkAlloc) && (size = discimg::loadFst(&image, checkAlloc, &fst)) != 0) {
			discimg::walkFst(fst, size, findBanner, &find);
			ok = find.found && find.length >= sizeof(bnr) && discimg::readDisc(&image, bnr, find.offset, sizeof(bnr));
		}
		if (fd >= 0) {
			ps3b_file_close(fd);
		}
		for (int i = 0; i < sCheckAllocCount; i++) {
			free(sCheckAllocs[i]);
		}
		sCheckAllocCount = 0;
	} else if (const char* root = ps3b_data_root()) {
		char path[300];
		snprintf(path, sizeof(path), "%s/files/opening.bnr", root);
		int fd = ps3b_file_open(path);
		if (fd >= 0) {
			ok = ps3b_file_pread(fd, bnr, 0, sizeof(bnr)) == (int)sizeof(bnr);
			ps3b_file_close(fd);
		}
	}
	if (!ok || !bannericon::isBanner(bnr, sizeof(bnr))) {
		return 0;
	}
	memcpy(out, bnr + bannericon::BANNER_OFFSET, bannericon::BANNER_BYTES);
	return 1;
}

extern "C" void ps3b_update_icon(void)
{
	const char* icon = GAME_DIR "/ICON0.PNG";
	char* current     = NULL;
	unsigned int size = 0;
	if (!ps3b_usrdir() || !ps3b_file_read_all(icon, &current, &size)) {
		return; // (not installed as a package)
	}
	const bool standIn = bannericon::hasStandInMark((const unsigned char*)current, size);
	free(current);
	if (!standIn) {
		return; // this package's own icon, or one made already
	}
	unsigned char* banner = (unsigned char*)malloc(bannericon::BANNER_BYTES + bannericon::BANNER_W * bannericon::BANNER_H * 4);
	unsigned char* rgba   = (unsigned char*)malloc(bannericon::ICON_W * bannericon::ICON_H * 4);
	unsigned char* png    = (unsigned char*)malloc(bannericon::PNG_BYTES);
	void* work            = malloc(bannericon::workBytes());
	if (!banner || !rgba || !png || !work) {
		bootLog("[boot] XMB icon: not made (out of memory)\n");
	} else if (!readBanner(banner)) {
		bootLog("[boot] XMB icon: not made (the disc's banner, opening.bnr, could not be read)\n");
	} else {
		unsigned char* bannerRgba = banner + bannericon::BANNER_BYTES;
		bannericon::decodeBanner(banner, bannerRgba);
		bannericon::makeIcon(bannerRgba, rgba, work);
		const unsigned bytes = bannericon::encodePng(rgba, png);
		const char* partial  = GAME_DIR "/ICON0.PNG.part";
		s32 fd               = -1;
		u64 written          = 0;
		int ok = sysLv2FsOpen(partial, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, NULL, 0) == 0;
		if (ok) {
			ok = sysLv2FsWrite(fd, png, bytes, &written) == 0 && written == bytes;
			sysLv2FsClose(fd);
		}
		if (ok) {
			sysLv2FsUnlink(icon);
			ok = sysLv2FsRename(partial, icon) == 0;
		}
		if (ok) {
			sysLv2FsChmod(icon, 0666);
			bootLog("[boot] XMB icon: made from the disc's banner (%u bytes; the XMB shows it once the game has quit)\n", bytes);
		} else {
			sysLv2FsUnlink(partial);
			bootLog("[boot] XMB icon: FAILED to write %s\n", icon);
		}
	}
	free(banner);
	free(rgba);
	free(png);
	free(work);
}

// ---------------------------------------------------------------- log file

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
static char sLogCopyPath[256]; // a second copy (the USB drive's PIKMIN folder), or ""
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

// (No logging in here: the log is being set up.)
static void copyQuietly(const char* from, const char* to)
{
	s32 in = -1, out = -1;
	if (sysLv2FsOpen(from, SYS_O_RDONLY, &in, 0, NULL, 0) != 0) {
		return;
	}
	if (sysLv2FsOpen(to, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &out, 0666, NULL, 0) == 0) {
		static char buf[64 * 1024];
		u64 got = 0, written = 0;
		while (sysLv2FsRead(in, buf, sizeof(buf), &got) == 0 && got > 0) {
			if (sysLv2FsWrite(out, buf, got, &written) != 0 || written != got) {
				break;
			}
		}
		sysLv2FsClose(out);
		sysLv2FsChmod(to, 0666);
	}
	sysLv2FsClose(in);
}

// Starts a new log file in `dir`, keeping the last one as pikmin_log_previous.txt (so that a crash's log is not
// lost by starting the game again before copying it). With `previousFrom`, the previous log is a copy of that
// one instead (the USB drive's copy of the log: the last session may have been played without the drive).
static int createLogFile(const char* dir, char* out, unsigned int outSize, const char* previousFrom = NULL)
{
	char path[256], previous[256];
	snprintf(path, sizeof(path), "%s/pikmin_log.txt", dir);
	snprintf(previous, sizeof(previous), "%s/pikmin_log_previous.txt", dir);
	if (previousFrom && ps3b_file_exists(previousFrom)) {
		sysLv2FsUnlink(previous);
		copyQuietly(previousFrom, previous);
	} else if (ps3b_file_exists(path)) {
		sysLv2FsUnlink(previous);
		sysLv2FsRename(path, previous);
	}
	s32 fd = -1;
	if (sysLv2FsOpen(path, SYS_O_WRONLY | SYS_O_CREAT | SYS_O_TRUNC, &fd, 0666, NULL, 0) != 0) {
		return 0;
	}
	sysLv2FsClose(fd);
	sysLv2FsChmod(path, 0666);
	snprintf(out, outSize, "%s", path);
	return 1;
}

static void chooseLogFile()
{
	// The game's folder on the hard drive, and a copy in the USB drive's PIKMIN folder (easier to get at).
	const char* usrdir = ps3b_usrdir();
	const char* usb    = ps3b_usb_folder();
	int have           = usrdir && createLogFile(usrdir, sLogPath, sizeof(sLogPath));
	if (usb) {
		if (!have) {
			have = createLogFile(usb, sLogPath, sizeof(sLogPath));
		} else {
			char previous[256];
			snprintf(previous, sizeof(previous), "%s/pikmin_log_previous.txt", usrdir);
			if (!createLogFile(usb, sLogCopyPath, sizeof(sLogCopyPath), previous)) {
				sLogCopyPath[0] = 0;
			}
		}
	}
	if (have) {
		sLogState = 1;
		return;
	}
	char path[256];
	if (const char* root = ps3b_data_root()) {
		if (createLogFile(root, sLogPath, sizeof(sLogPath))) {
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
		if (createLogFile(dev, sLogPath, sizeof(sLogPath))) {
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
	const char* paths[2] = { sLogPath, sLogCopyPath };
	for (int i = 0; i < 2; i++) {
		s32 fd = -1;
		if (!paths[i][0] || sysLv2FsOpen(paths[i], SYS_O_WRONLY, &fd, 0, NULL, 0) != 0) {
			continue;
		}
		u64 pos = 0, written = 0;
		sysLv2FsLSeek64(fd, 0, 2 /* SEEK_END */, &pos);
		sysLv2FsWrite(fd, text, len, &written);
		sysLv2FsClose(fd);
	}
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

extern "C" const char* ps3b_log_copy_path(void)
{
	return sLogState == 1 && sLogCopyPath[0] ? sLogCopyPath : NULL;
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
