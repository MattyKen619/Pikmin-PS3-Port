// PS3 port: host check of banner_icon.h's PNG writer (build 55): many pictures of different kinds are encoded,
// then read back with zlib (the reference inflate) and the PNG rules, and must match exactly.
//
//   make -C tools/ps3/disc_test png_selftest && tools/ps3/disc_test/png_selftest [OUT_DIR]
//
// With OUT_DIR, the PNGs are written there too (for checking with other decoders: png_selftest.py).

#include "ps3/banner_icon.h"

#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

using namespace bannericon;

static unsigned get32(const unsigned char* p) { return ((unsigned)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// Reads the PNG back; 1 if it is well formed and its pixels are `rgba`.
static int check(const unsigned char* png, unsigned size, const unsigned char* rgba, char* why)
{
	static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	if (size < 8 || memcmp(png, sig, 8) != 0) {
		return strcpy(why, "signature"), 0;
	}
	static unsigned char z[PNG_BYTES];
	unsigned zlen = 0, p = 8, chunks = 0;
	bool end = false;
	while (p + 12 <= size && !end) {
		const unsigned len = get32(png + p);
		if (p + 12 + len > size || crc32(png + p + 4, len + 4, 0) != get32(png + p + 8 + len)) {
			return sprintf(why, "chunk %u: length or crc", chunks), 0;
		}
		if (!memcmp(png + p + 4, "IHDR", 4)) {
			if (len != 13 || get32(png + p + 8) != ICON_W || get32(png + p + 12) != ICON_H || png[p + 16] != 8 || png[p + 17] != 6) {
				return strcpy(why, "IHDR"), 0;
			}
		} else if (!memcmp(png + p + 4, "IDAT", 4)) {
			if (len > IDAT_PART) {
				return strcpy(why, "IDAT larger than 8 KB"), 0;
			}
			memcpy(z + zlen, png + p + 8, len);
			zlen += len;
		} else if (!memcmp(png + p + 4, "IEND", 4)) {
			end = true;
		}
		p += 12 + len;
		chunks++;
	}
	if (!end || p != size) {
		return strcpy(why, "no IEND at the end"), 0;
	}
	static unsigned char raw[RAW_BYTES + 1];
	uLongf rawLen = sizeof(raw);
	if (uncompress(raw, &rawLen, z, zlen) != Z_OK || rawLen != RAW_BYTES) {
		return sprintf(why, "zlib: inflate failed or wrong size (%lu)", (unsigned long)rawLen), 0;
	}
	static unsigned char img[ICON_W * ICON_H * 4];
	const int row = ICON_W * 4;
	for (int y = 0; y < ICON_H; y++) {
		const int f = raw[y * (row + 1)];
		if (f > 4) {
			return strcpy(why, "filter type"), 0;
		}
		for (int i = 0; i < row; i++) {
			const int a = i >= 4 ? img[y * row + i - 4] : 0, b = y ? img[(y - 1) * row + i] : 0,
			          c = (i >= 4 && y) ? img[(y - 1) * row + i - 4] : 0;
			const int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) >> 1 : paeth(a, b, c);
			img[y * row + i] = (unsigned char)(raw[y * (row + 1) + 1 + i] + pred);
		}
	}
	if (memcmp(img, rgba, sizeof(img)) != 0) {
		return strcpy(why, "pixels differ"), 0;
	}
	return 1;
}

static unsigned rnd(unsigned* s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

int main(int argc, char** argv)
{
	static unsigned char rgba[ICON_W * ICON_H * 4];
	static unsigned char png[PNG_BYTES];
	int bad = 0, kinds[6] = { 0 };
	unsigned smallest = ~0u, largest = 0;
	for (int t = 0; t < 120; t++) {
		unsigned s = 1234567u + t * 7919u;
		const int kind = t % 6;
		kinds[kind]++;
		for (int y = 0; y < ICON_H; y++) {
			for (int x = 0; x < ICON_W; x++) {
				unsigned char* o = rgba + (y * ICON_W + x) * 4;
				switch (kind) {
				case 0: // noise (incompressible)
					for (int c = 0; c < 4; c++) o[c] = (unsigned char)rnd(&s);
					break;
				case 1: // all transparent
					memset(o, 0, 4);
					break;
				case 2: // gradients
					o[0] = (unsigned char)(x * 255 / ICON_W), o[1] = (unsigned char)(y * 255 / ICON_H), o[2] = (unsigned char)(x + y), o[3] = 255;
					break;
				case 3: // a band of noise in a transparent field, like the banner
					if (y >= 34 && y < 141) for (int c = 0; c < 4; c++) o[c] = (unsigned char)(rnd(&s) & (t & 8 ? 0xF0 : 0xFF));
					else memset(o, 0, 4);
					break;
				case 4: // few colours, long runs
					o[0] = o[1] = o[2] = (unsigned char)((x / 37 + y / 11) % 3 * 100), o[3] = (unsigned char)(x % 5 ? 255 : 0);
					break;
				default: // repeats at many distances
					o[0] = (unsigned char)((x * x + y) % 251), o[1] = (unsigned char)((x * 3) ^ y), o[2] = (unsigned char)(rnd(&s) % 4), o[3] = 200;
					break;
				}
			}
		}
		bool compressed      = false;
		const unsigned bytes = encodePng(rgba, png, &compressed);
		char why[128]        = "";
		if (!bytes || !check(png, bytes, rgba, why)) {
			printf("picture %d (kind %d): %s\n", t, kind, bytes ? why : "no memory");
			bad++;
		}
		smallest = bytes < smallest ? bytes : smallest;
		largest  = bytes > largest ? bytes : largest;
		if (argc > 1 && t < 12) {
			char path[512];
			snprintf(path, sizeof(path), "%s/selftest_%02d.png", argv[1], t);
			FILE* f = fopen(path, "wb");
			fwrite(png, 1, bytes, f);
			fclose(f);
			snprintf(path, sizeof(path), "%s/selftest_%02d.rgba", argv[1], t);
			f = fopen(path, "wb");
			fwrite(rgba, 1, sizeof(rgba), f);
			fclose(f);
		}
	}
	printf("120 pictures: %d wrong; sizes %u to %u bytes\n", bad, smallest, largest);
	return bad ? 1 : 0;
}
