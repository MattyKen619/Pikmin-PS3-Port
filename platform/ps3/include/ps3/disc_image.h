// PS3 port: reading the game's files straight from a GameCube disc image (build 52), so that the game can be
// played from one file on the hard drive instead of an extracted folder.
//
// Two formats:
//   - a plain image (ISO/GCM), trimmed or not (a read past its end gives zeros);
//   - a CISO ("compact ISO"): a 32 KB header ("CISO", the block size as a little-endian word, then one byte per
//     block: 1 if the block is stored), followed by the stored blocks in order. Blocks not stored read as zeros.
// The disc itself: the game ID at 0 ("GPIE01" for the North American release), the revision at 7, the magic word
// 0xC2339F3D at 0x1C, and the file table's offset and size at 0x424. File table entries are 12 bytes: a flag
// byte (directory) and a 24-bit name offset, then for a file its disc offset and length, for a directory its
// parent and the index after its last entry. Entry numbers are the game's DVD entry numbers (DVDFastOpen).
//
// Plain C types only: used by the PS3 system side (the first-boot copy, ps3_backend.cpp), the game side (the DVD
// layer, dvd.cpp) and a host test (tools/ps3/disc_test).

#ifndef PS3_DISC_IMAGE_H
#define PS3_DISC_IMAGE_H

#include <string.h>

namespace discimg {

// Reads len bytes at offset of the image file into dst; returns the number of bytes read (fewer at its end) or -1.
typedef int (*ReadFn)(void* ctx, void* dst, unsigned long long offset, unsigned int len);
typedef void* (*AllocFn)(unsigned int size);

enum { CISO_HEADER = 0x8000, CISO_MAP_ENTRIES = CISO_HEADER - 8, DISC_MAGIC = 0xC2339F3Du };

struct Image {
	ReadFn read;
	void* ctx;
	int ciso;
	unsigned int blockSize;
	unsigned int blockCount; // map entries in use (CISO)
	int* blockPos;           // CISO: each block's place among the stored ones, or -1
};

static inline unsigned int be32(const unsigned char* p)
{
	return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

// Reads from the image file, zeros past its end. False on a read error.
static inline bool readFile(const Image* im, void* dst, unsigned long long offset, unsigned int len)
{
	unsigned char* out = (unsigned char*)dst;
	while (len) {
		const int got = im->read(im->ctx, out, offset, len);
		if (got < 0) {
			return false;
		}
		if (got == 0) {
			memset(out, 0, len);
			return true;
		}
		out += got;
		offset += (unsigned int)got;
		len -= (unsigned int)got;
	}
	return true;
}

// Reads disc bytes [offset, offset + len). False on a read error.
static inline bool readDisc(const Image* im, void* dst, unsigned long long offset, unsigned int len)
{
	if (!im->ciso) {
		return readFile(im, dst, offset, len);
	}
	unsigned char* out = (unsigned char*)dst;
	while (len) {
		const unsigned long long block = offset / im->blockSize;
		const unsigned int inner       = (unsigned int)(offset % im->blockSize);
		unsigned int take              = im->blockSize - inner;
		if (take > len) {
			take = len;
		}
		const int pos = block < im->blockCount ? im->blockPos[block] : -1;
		if (pos < 0) {
			memset(out, 0, take);
		} else if (!readFile(im, out, CISO_HEADER + (unsigned long long)pos * im->blockSize + inner, take)) {
			return false;
		}
		out += take;
		offset += take;
		len -= take;
	}
	return true;
}

// Sets up `im` for an image file. Returns 0, or a short reason it is not a usable image.
static inline const char* open(Image* im, ReadFn read, void* ctx, AllocFn alloc)
{
	memset(im, 0, sizeof(*im));
	im->read = read;
	im->ctx  = ctx;
	unsigned char head[16];
	if (!readFile(im, head, 0, sizeof(head))) {
		return "cannot be read";
	}
	if (memcmp(head, "CISO", 4) == 0) {
		unsigned char* map = (unsigned char*)alloc(CISO_MAP_ENTRIES);
		if (!map) {
			return "out of memory";
		}
		if (!readFile(im, map, 8, CISO_MAP_ENTRIES)) {
			return "cannot be read";
		}
		im->blockSize = head[4] | (head[5] << 8) | (head[6] << 16) | ((unsigned int)head[7] << 24);
		if (im->blockSize < 0x8000 || (im->blockSize & (im->blockSize - 1))) {
			return "a CISO with an unusual block size";
		}
		unsigned int used = 0;
		for (unsigned int i = 0; i < CISO_MAP_ENTRIES; i++) {
			if (map[i]) {
				used = i + 1;
			}
		}
		im->blockPos = (int*)alloc(used ? used * sizeof(int) : sizeof(int));
		if (!im->blockPos) {
			return "out of memory";
		}
		int n = 0;
		for (unsigned int i = 0; i < used; i++) {
			im->blockPos[i] = map[i] ? n++ : -1;
		}
		im->blockCount = used;
		im->ciso       = 1;
	}
	unsigned char disc[0x440];
	if (!readDisc(im, disc, 0, sizeof(disc))) {
		return "cannot be read";
	}
	if (be32(disc + 0x1C) != DISC_MAGIC) {
		return "not a GameCube disc image";
	}
	return 0;
}

// The disc header's game ID (6 characters + NUL) and revision.
static inline bool gameId(const Image* im, char id[7], int* revision)
{
	unsigned char disc[8];
	if (!readDisc(im, disc, 0, sizeof(disc))) {
		return false;
	}
	memcpy(id, disc, 6);
	id[6]     = 0;
	*revision = disc[7];
	return true;
}

// Loads the file table (alloc'd). Returns its size, or 0.
static inline unsigned int loadFst(const Image* im, AllocFn alloc, unsigned char** out)
{
	unsigned char disc[0x440];
	if (!readDisc(im, disc, 0, sizeof(disc))) {
		return 0;
	}
	const unsigned int offset = be32(disc + 0x424), size = be32(disc + 0x428);
	if (size < 12 || size > (16u << 20)) {
		return 0;
	}
	unsigned char* fst = (unsigned char*)alloc(size + 1);
	if (!fst || !readDisc(im, fst, offset, size)) {
		return 0;
	}
	fst[size] = 0;
	const unsigned int count = be32(fst + 8);
	if (count == 0 || (unsigned long long)count * 12 > size) {
		return 0;
	}
	*out = fst;
	return size;
}

// Calls visit(user, index, isDir, path, discOffset, length) for each entry after the root, in order, with the
// path built from the directories it is in ("/dir/file"). Returns the entry count (the root included), or 0.
typedef void (*VisitFn)(void* user, unsigned int index, bool isDir, const char* path, unsigned int offset, unsigned int length);

static inline unsigned int walkFst(const unsigned char* fst, unsigned int size, VisitFn visit, void* user)
{
	const unsigned int count = be32(fst + 8);
	if (count == 0 || (unsigned long long)count * 12 > size) {
		return 0;
	}
	const char* strings = (const char*)fst + count * 12;
	const unsigned int stringsSize = size - count * 12;
	enum { MAX_DEPTH = 32 };
	unsigned int dirEnd[MAX_DEPTH];
	unsigned int dirLen[MAX_DEPTH]; // length of the path prefix for that directory
	char path[512];
	int depth   = 0;
	unsigned int len = 0;
	for (unsigned int i = 1; i < count; i++) {
		while (depth > 0 && i >= dirEnd[depth - 1]) {
			depth--;
			len = depth ? dirLen[depth - 1] : 0;
		}
		const unsigned char* e  = fst + i * 12;
		const unsigned int w0   = be32(e), w1 = be32(e + 4), w2 = be32(e + 8);
		const bool isDir        = (w0 >> 24) != 0;
		const unsigned int name = w0 & 0xFFFFFF;
		const char* nm          = name < stringsSize ? strings + name : "";
		path[len]               = '/';
		unsigned int n          = len + 1;
		for (; *nm && n < sizeof(path) - 1; nm++) {
			path[n++] = *nm;
		}
		path[n] = 0;
		visit(user, i, isDir, path, isDir ? 0 : w1, isDir ? 0 : w2);
		if (isDir && depth < MAX_DEPTH) {
			dirEnd[depth] = w2;
			dirLen[depth] = n;
			depth++;
			len = n;
		}
	}
	return count;
}

} // namespace discimg

#endif
