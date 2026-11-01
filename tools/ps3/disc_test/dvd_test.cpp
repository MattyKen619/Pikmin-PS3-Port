// PS3 port: host check of the DVD layer's disc image mode (platform/ps3/sdk/dvd.cpp, build 52). Opens every
// file of your own disc image through the game's DVD calls (by path, in other letter cases and with "..", and
// by entry number), reads it in pieces of several sizes, and prints the same list as disc_test, for check.py.
//
//   make -C tools/ps3/disc_test dvd_test && tools/ps3/disc_test/dvd_test IMAGE > list.txt
//
// The PS3 services dvd.cpp uses are stood in for here with the host's own (files, memory); there are no other
// threads, so every read takes dvd.cpp's direct path (the one used on a game thread that cannot wait).
//
// Never share the image or files read from it; they are Nintendo's copyrighted game data.

#include "ps3/disc_image.h"
#include "../../../platform/ps3/backend/ps3_backend.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
void shimInit(void);
int shimPathToEntry(const char* path);
int shimOpen(const char* path, unsigned int* length);
int shimFastOpen(int entry, unsigned int* length);
int shimRead(void* buf, int length, int offset);
int shimReadAsync(void* buf, int length, int offset);
}

static const char* sImagePath;
static int sQuiet;

// ---- stand-ins for the rest of the port (sdk_internal.h, os_sched.cpp) and the PS3 backend

extern "C" {
void ps3Log(const char* fmt, ...)
{
	if (sQuiet) {
		return;
	}
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	va_end(a);
}
void ps3LogOnce(int* flag, const char* fmt, ...)
{
	if (*flag) {
		return;
	}
	*flag = 1;
	va_list a;
	va_start(a, fmt);
	vfprintf(stderr, fmt, a);
	va_end(a);
}
void ps3NoteActivity(const char*) { }
void ps3PostInterrupt(void (*fn)(void*), void* arg) { fn(arg); }
void ps3DeliverInterrupts(void) { }
void ps3WaitOn(void*, const char*) { }
void ps3WakeAll(void*) { }
int ps3InInterrupt(void) { return 0; }
int ps3CanBlock(void) { return 0; }
int ps3CanYield(void) { return 0; }

static int sLocks;
int ps3b_mutex_create(unsigned int* out) { *out = 1; return 0; }
void ps3b_mutex_lock(unsigned int) { if (sLocks++) { fprintf(stderr, "lock taken twice\n"); exit(2); } }
void ps3b_mutex_unlock(unsigned int) { sLocks--; }
int ps3b_cond_create(unsigned int* out, unsigned int) { *out = 1; return 0; }
int ps3b_cond_wait(unsigned int, unsigned int) { return 1; }
void ps3b_cond_signal(unsigned int) { }
void ps3b_cond_broadcast(unsigned int) { }
int ps3b_thread_create(void (*)(void*), void*, int, unsigned int, const char*) { return -1; } // (no I/O thread)
void* ps3b_memalign(unsigned int align, unsigned int size)
{
	void* p = 0;
	return posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size) == 0 ? p : 0;
}
void ps3b_free(void* p) { free(p); }
int ps3b_file_exists(const char* path) { struct stat st; return stat(path, &st) == 0; }
int ps3b_file_open(const char* path) { return open(path, O_RDONLY); }
long long ps3b_file_size(int fd) { struct stat st; return fstat(fd, &st) == 0 ? (long long)st.st_size : -1; }
int ps3b_file_pread(int fd, void* buf, unsigned long long offset, unsigned int len) { return (int)pread(fd, buf, len, (off_t)offset); }
void ps3b_file_close(int fd) { close(fd); }
int ps3b_file_read_all(const char*, char**, unsigned int*) { return 0; }
const char* ps3b_data_root(void) { return 0; }
const char* ps3b_disc_image(void) { return sImagePath; }
}

// ---- the check

static unsigned int crc32(const unsigned char* p, unsigned int n, unsigned int crc)
{
	crc = ~crc;
	while (n--) {
		crc ^= *p++;
		for (int k = 0; k < 8; k++) {
			crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
		}
	}
	return ~crc;
}

static unsigned char* sBuf;
static int sErrors, sFiles;

static void fail(const char* what, const char* path)
{
	if (++sErrors <= 20) {
		fprintf(stderr, "FAILED: %s: %s\n", what, path);
	}
}

static void visit(void*, unsigned int index, bool isDir, const char* path, unsigned int, unsigned int length)
{
	// Look-ups: as is, in capitals (ASCII letters), and with a detour through "..".
	char other[600];
	if (shimPathToEntry(path) != (int)index) {
		fail("look-up", path);
	}
	snprintf(other, sizeof(other), "%s", path);
	for (char* p = other; *p; p++) {
		if (*p >= 'a' && *p <= 'z') {
			*p = (char)(*p - 'a' + 'A');
		}
	}
	if (shimPathToEntry(other) != (int)index) {
		fail("look-up in capitals", path);
	}
	snprintf(other, sizeof(other), "/zz/..%s", path);
	if (shimPathToEntry(other) != (int)index) {
		fail("look-up through ..", path);
	}
	if (isDir) {
		printf("%u\td\t%s\n", index, path);
		return;
	}
	unsigned int len1 = 0, len2 = 0;
	if (!shimOpen(path, &len1) || len1 != length || !shimFastOpen((int)index, &len2) || len2 != length) {
		fail("open", path);
	}
	// Whole file in pieces of a size that changes (32-byte multiples, as the game reads), the last one rounded up
	// to 32 bytes: the bytes after the end must read as zeros. Every 4th file through the async call.
	static const unsigned int pieces[] = { 0x20, 0x8000, 0x12340, 0x100000, 0x3FFFE0, 0x200 };
	const unsigned int padded = (length + 31) & ~31u;
	unsigned int done = 0, k = index % 6;
	unsigned int crc = 0;
	while (done < padded) {
		unsigned int take = pieces[k++ % 6];
		if (take > padded - done) {
			take = padded - done;
		}
		memset(sBuf, 0xAA, take);
		const int got = (sFiles % 4 == 3) ? shimReadAsync(sBuf, (int)take, (int)done) : shimRead(sBuf, (int)take, (int)done);
		if (got != (int)take) {
			fail("read", path);
			break;
		}
		const unsigned int real = done + take <= length ? take : (done < length ? length - done : 0);
		crc = crc32(sBuf, real, crc);
		for (unsigned int i = real; i < take; i++) {
			if (sBuf[i] != 0) {
				fail("zeros after the end", path);
				break;
			}
		}
		done += take;
	}
	sFiles++;
	printf("%u\tf\t%s\t%u\t%08x\n", index, path, length, crc);
}

static int readAt(void* ctx, void* dst, unsigned long long offset, unsigned int len)
{
	return (int)pread(*(int*)ctx, dst, len, (off_t)offset);
}

static void* alloc(unsigned int size) { return malloc(size); }

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: dvd_test IMAGE > list.txt\n");
		return 1;
	}
	sImagePath = argv[1];
	shimInit();
	sQuiet = 1; // (dvd.cpp logs every open)

	// The file table, read independently, for the list of paths to try.
	int fd = open(argv[1], O_RDONLY);
	discimg::Image image;
	unsigned char* fst = 0;
	unsigned int size  = 0;
	if (fd < 0 || discimg::open(&image, readAt, &fd, alloc) || !(size = discimg::loadFst(&image, alloc, &fst))) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	sBuf = (unsigned char*)malloc(4u << 20);
	unsigned int count = discimg::walkFst(fst, size, visit, 0);
	if (shimPathToEntry("/no/such/file.bin") != -1) {
		fail("look-up of a missing file", "/no/such/file.bin");
	}
	fprintf(stderr, "%u entries, %d files read through the DVD layer, %d problems\n", count, sFiles, sErrors);
	return sErrors ? 1 : 0;
}
