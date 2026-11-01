// PS3 port: host check of platform/ps3/include/ps3/disc_image.h (build 52). Lists every file of your own disc
// image (ISO/GCM/CISO) with its size and a checksum of its contents as read through the reader the PS3 build
// uses; tools/ps3/disc_test/check.py reads the same image independently and compares.
//
//   g++ -O2 -I../../../platform/ps3/include -o disc_test disc_test.cpp && ./disc_test IMAGE > list.txt
//
// Never share the image or files read from it; they are Nintendo's copyrighted game data.

#include "ps3/disc_image.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int readAt(void* ctx, void* dst, unsigned long long offset, unsigned int len)
{
	return (int)pread(*(int*)ctx, dst, len, (off_t)offset);
}

static void* alloc(unsigned int size) { return malloc(size); }

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

static discimg::Image sImage;
static unsigned char* sBuf;
static int sErrors;

static void visit(void*, unsigned int index, bool isDir, const char* path, unsigned int offset, unsigned int length)
{
	if (isDir) {
		printf("%u\td\t%s\n", index, path);
		return;
	}
	unsigned int crc = 0;
	for (unsigned int done = 0; done < length;) {
		unsigned int take = length - done < (4u << 20) ? length - done : (4u << 20);
		if (!discimg::readDisc(&sImage, sBuf, (unsigned long long)offset + done, take)) {
			sErrors++;
			break;
		}
		crc = crc32(sBuf, take, crc);
		done += take;
	}
	printf("%u\tf\t%s\t%u\t%08x\n", index, path, length, crc);
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: disc_test IMAGE\n");
		return 1;
	}
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	const char* why = discimg::open(&sImage, readAt, &fd, alloc);
	if (why) {
		fprintf(stderr, "%s: %s\n", argv[1], why);
		return 1;
	}
	char id[7];
	int rev = 0;
	discimg::gameId(&sImage, id, &rev);
	unsigned char* fst = 0;
	unsigned int size  = discimg::loadFst(&sImage, alloc, &fst);
	if (!size) {
		fprintf(stderr, "no file table\n");
		return 1;
	}
	sBuf = (unsigned char*)malloc(4u << 20);
	unsigned int count = discimg::walkFst(fst, size, visit, 0);
	fprintf(stderr, "%s rev %d, %s, %u entries, %d read errors\n", id, rev, sImage.ciso ? "CISO" : "plain image", count, sErrors);
	return sErrors ? 1 : 0;
}
