// PS3 port: host check of platform/ps3/include/ps3/banner_icon.h (build 54). Makes the XMB icon from your own
// disc image's banner exactly as the game does on the PS3; icon_check.py compares it with the same picture made
// independently (Python, Pillow).
//
//   make -C tools/ps3/disc_test icon_test
//   tools/ps3/disc_test/icon_test IMAGE icon.png && python3 tools/ps3/disc_test/icon_check.py IMAGE icon.png
//   tools/ps3/disc_test/icon_test --mark FILE.png    (exit 0 if FILE.png carries the stand-in mark)
//
// Never share the image, the icon or anything else made from the disc; they are Nintendo's game data.

#include "ps3/banner_icon.h"
#include "ps3/disc_image.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

static int readAt(void* ctx, void* dst, unsigned long long offset, unsigned int len)
{
	return (int)pread(*(int*)ctx, dst, len, (off_t)offset);
}

static void* alloc(unsigned int size) { return malloc(size); }

struct Find {
	unsigned offset, length;
	int found;
};

static void visit(void* user, unsigned int, bool isDir, const char* path, unsigned int offset, unsigned int length)
{
	Find* f = (Find*)user;
	if (!isDir && !f->found && strcasecmp(path, "/opening.bnr") == 0) {
		f->offset = offset;
		f->length = length;
		f->found  = 1;
	}
}

int main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "--mark") == 0) {
		FILE* f = fopen(argv[2], "rb");
		static unsigned char buf[1 << 20];
		const unsigned n = f ? (unsigned)fread(buf, 1, sizeof(buf), f) : 0;
		const bool marked = bannericon::hasStandInMark(buf, n);
		printf("%s: %s\n", argv[2], marked ? "marked as a stand-in icon" : "not marked");
		return marked ? 0 : 1;
	}
	if (argc != 3) {
		fprintf(stderr, "usage: icon_test IMAGE OUT.png | icon_test --mark FILE.png\n");
		return 1;
	}
	int fd = open(argv[1], O_RDONLY);
	discimg::Image image;
	unsigned char* fst = 0;
	unsigned size      = 0;
	Find find          = { 0, 0, 0 };
	if (fd < 0 || discimg::open(&image, readAt, &fd, alloc) || !(size = discimg::loadFst(&image, alloc, &fst))) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	discimg::walkFst(fst, size, visit, &find);
	static unsigned char bnr[bannericon::BANNER_OFFSET + bannericon::BANNER_BYTES];
	if (!find.found || !discimg::readDisc(&image, bnr, find.offset, sizeof(bnr)) || !bannericon::isBanner(bnr, sizeof(bnr))) {
		fprintf(stderr, "no banner\n");
		return 1;
	}
	static unsigned char banner[bannericon::BANNER_W * bannericon::BANNER_H * 4];
	static unsigned char icon[bannericon::ICON_W * bannericon::ICON_H * 4];
	static unsigned char png[bannericon::PNG_BYTES];
	void* work = malloc(bannericon::workBytes());
	bannericon::decodeBanner(bnr + bannericon::BANNER_OFFSET, banner);
	bannericon::makeIcon(banner, icon, work);
	const unsigned bytes = bannericon::encodePng(icon, png);
	FILE* out = fopen(argv[2], "wb");
	if (!out || fwrite(png, 1, bytes, out) != bytes) {
		perror(argv[2]);
		return 1;
	}
	fclose(out);
	printf("%s: %u bytes (room for %u)\n", argv[2], bytes, (unsigned)bannericon::PNG_BYTES);
	return bytes == (unsigned)bannericon::PNG_BYTES ? 0 : 1;
}
