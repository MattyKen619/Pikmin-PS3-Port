// PS3 port: the XMB icon made from the game's own disc banner (build 54).
//
// The GameCube menu shows a 96 x 32 picture for each disc: the banner, in /opening.bnr ("BNR1" or "BNR2", then
// the picture at 0x20 in the RGB5A3 format, in 4 x 4 tiles). A package built without an icon of its own carries
// a stand-in icon marked as such (tools/ps3/mark_icon.py); on start, the game replaces it with this banner,
// scaled to the XMB's 320 x 176 and saved as a PNG. Every player's icon comes from their own disc, and the
// repository holds no game artwork.
//
// Plain C types only: used by the PS3 backend (ps3_backend.cpp) and a host test (tools/ps3/disc_test).

#ifndef PS3_BANNER_ICON_H
#define PS3_BANNER_ICON_H

#include <math.h>
#include <string.h>

namespace bannericon {

enum {
	BANNER_W = 96, BANNER_H = 32,
	BANNER_OFFSET = 0x20,                      // in opening.bnr
	BANNER_BYTES  = BANNER_W * BANNER_H * 2,   // RGB5A3
	ICON_W = 320, ICON_H = 176,
	RAW_BYTES = ICON_H * (1 + ICON_W * 4),     // PNG rows, each with its filter byte
	STORED_BLOCK = 65535,
	PNG_BYTES = 8 + 25 + 12 + 2 + RAW_BYTES + 5 * ((RAW_BYTES + STORED_BLOCK - 1) / STORED_BLOCK) + 4 + 12, // see encodePng
};

// The mark that a package's icon is a stand-in to be replaced (a PNG text chunk: keyword, NUL, text).
static const char STANDIN_MARK[]       = "pikmin-ps3\0icon-from-disc";
static const unsigned STANDIN_MARK_LEN = sizeof(STANDIN_MARK) - 1;

static inline bool hasStandInMark(const unsigned char* data, unsigned size)
{
	for (unsigned i = 0; i + STANDIN_MARK_LEN <= size; i++) {
		if (data[i] == 'p' && memcmp(data + i, STANDIN_MARK, STANDIN_MARK_LEN) == 0) {
			return true;
		}
	}
	return false;
}

static inline bool isBanner(const unsigned char* bnr, unsigned size)
{
	return size >= BANNER_OFFSET + BANNER_BYTES && memcmp(bnr, "BNR", 3) == 0 && (bnr[3] == '1' || bnr[3] == '2');
}

// The banner's picture (RGB5A3, 4 x 4 tiles) as RGBA, 8 bits a channel.
static inline void decodeBanner(const unsigned char* src, unsigned char* rgba)
{
	unsigned p = 0;
	for (int ty = 0; ty < BANNER_H; ty += 4) {
		for (int tx = 0; tx < BANNER_W; tx += 4) {
			for (int y = ty; y < ty + 4; y++) {
				for (int x = tx; x < tx + 4; x++, p += 2) {
					const unsigned v = ((unsigned)src[p] << 8) | src[p + 1];
					unsigned char* o = rgba + (y * BANNER_W + x) * 4;
					if (v & 0x8000) { // 5:5:5, opaque
						const unsigned r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
						o[0] = (unsigned char)((r << 3) | (r >> 2));
						o[1] = (unsigned char)((g << 3) | (g >> 2));
						o[2] = (unsigned char)((b << 3) | (b >> 2));
						o[3] = 255;
					} else { // 3:4:4:4, alpha first
						const unsigned a = (v >> 12) & 7;
						o[0] = (unsigned char)(((v >> 8) & 15) * 17);
						o[1] = (unsigned char)(((v >> 4) & 15) * 17);
						o[2] = (unsigned char)((v & 15) * 17);
						o[3] = (unsigned char)((a << 5) | (a << 2) | (a >> 1));
					}
				}
			}
		}
	}
}

// Lanczos-3 weights for scaling `inSize` samples to `outSize`, for output sample i: taps at first + k.
struct Taps {
	int first[ICON_W];
	float w[ICON_W][6];
};

static inline float lanczos3(float x)
{
	if (x < 0) {
		x = -x;
	}
	if (x < 1e-6f) {
		return 1.0f;
	}
	if (x >= 3.0f) {
		return 0.0f;
	}
	const float px = 3.14159265f * x;
	return 3.0f * sinf(px) * sinf(px / 3.0f) / (px * px);
}

static inline void makeTaps(Taps* t, int outSize, float scale)
{
	for (int i = 0; i < outSize; i++) {
		const float center = (i + 0.5f) / scale - 0.5f;
		const int first    = (int)floorf(center) - 2;
		float sum          = 0;
		for (int k = 0; k < 6; k++) {
			t->w[i][k] = lanczos3(center - (float)(first + k));
			sum += t->w[i][k];
		}
		for (int k = 0; k < 6; k++) {
			t->w[i][k] /= sum;
		}
		t->first[i] = first;
	}
}

// The XMB icon (ICON_W x ICON_H RGBA): the banner scaled to the full width, centred, transparent around it.
// Scaled with premultiplied alpha (see-through pixels don't tint the edges); outside the banner is transparent.
// `work` needs workBytes().
static inline unsigned workBytes() { return 2 * sizeof(Taps) + BANNER_H * ICON_W * 4 * sizeof(float); }

static inline void makeIcon(const unsigned char* banner, unsigned char* icon, void* work)
{
	const float scale = (float)ICON_W / BANNER_W;
	const int outH    = (int)(BANNER_H * scale + 0.5f); // 107
	const int top     = (ICON_H - outH) / 2;
	Taps* tx          = (Taps*)work;
	Taps* ty          = tx + 1;
	float* rows       = (float*)(ty + 1); // BANNER_H rows scaled across, premultiplied
	makeTaps(tx, ICON_W, scale);
	makeTaps(ty, outH, scale);
	for (int y = 0; y < BANNER_H; y++) {
		for (int x = 0; x < ICON_W; x++) {
			float acc[4] = { 0, 0, 0, 0 };
			for (int k = 0; k < 6; k++) {
				const int sx = tx->first[x] + k;
				if (sx < 0 || sx >= BANNER_W) {
					continue;
				}
				const unsigned char* s = banner + (y * BANNER_W + sx) * 4;
				const float a = s[3] / 255.0f, w = tx->w[x][k];
				acc[0] += w * s[0] * a;
				acc[1] += w * s[1] * a;
				acc[2] += w * s[2] * a;
				acc[3] += w * s[3];
			}
			memcpy(rows + (y * ICON_W + x) * 4, acc, sizeof(acc));
		}
	}
	memset(icon, 0, ICON_W * ICON_H * 4);
	for (int y = 0; y < outH; y++) {
		for (int x = 0; x < ICON_W; x++) {
			float acc[4] = { 0, 0, 0, 0 };
			for (int k = 0; k < 6; k++) {
				const int sy = ty->first[y] + k;
				if (sy < 0 || sy >= BANNER_H) {
					continue;
				}
				const float* s = rows + (sy * ICON_W + x) * 4;
				const float w  = ty->w[y][k];
				for (int c = 0; c < 4; c++) {
					acc[c] += w * s[c];
				}
			}
			unsigned char* o = icon + ((top + y) * ICON_W + x) * 4;
			float a          = acc[3];
			a                = a < 0 ? 0 : a > 255 ? 255 : a;
			o[3]             = (unsigned char)(a + 0.5f);
			for (int c = 0; c < 3; c++) {
				float v = a > 0.5f ? acc[c] * 255.0f / a : 0; // (back from premultiplied)
				v       = v < 0 ? 0 : v > 255 ? 255 : v;
				o[c]    = o[3] ? (unsigned char)(v + 0.5f) : 0;
			}
		}
	}
}

// ---- PNG (8-bit RGBA, no filtering, deflate "stored" blocks: simple, and about 220 KB)

static inline unsigned crc32(const unsigned char* p, unsigned n, unsigned crc)
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

static inline void put32(unsigned char* p, unsigned v)
{
	p[0] = (unsigned char)(v >> 24);
	p[1] = (unsigned char)(v >> 16);
	p[2] = (unsigned char)(v >> 8);
	p[3] = (unsigned char)v;
}

// Writes the PNG of an ICON_W x ICON_H RGBA picture to out (PNG_BYTES). Returns its size.
static inline unsigned encodePng(const unsigned char* rgba, unsigned char* out)
{
	static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	unsigned char* p = out;
	memcpy(p, sig, 8);
	p += 8;
	// IHDR
	put32(p, 13);
	memcpy(p + 4, "IHDR", 4);
	put32(p + 8, ICON_W);
	put32(p + 12, ICON_H);
	p[16] = 8; // bits per channel
	p[17] = 6; // RGBA
	p[18] = p[19] = p[20] = 0;
	put32(p + 21, crc32(p + 4, 17, 0));
	p += 25;
	// IDAT: a zlib stream of stored blocks
	unsigned char* idat = p;
	p += 8;
	*p++ = 0x78;
	*p++ = 0x01;
	unsigned adlerA = 1, adlerB = 0;
	unsigned left = RAW_BYTES, row = 0, col = 0; // col: byte within the row (0 = the filter byte)
	while (left) {
		const unsigned n = left < STORED_BLOCK ? left : STORED_BLOCK;
		left -= n;
		*p++ = left ? 0 : 1;
		*p++ = (unsigned char)n;
		*p++ = (unsigned char)(n >> 8);
		*p++ = (unsigned char)~n;
		*p++ = (unsigned char)(~n >> 8);
		for (unsigned i = 0; i < n; i++) {
			const unsigned char b = col == 0 ? 0 : rgba[row * ICON_W * 4 + col - 1];
			*p++ = b;
			adlerA = (adlerA + b) % 65521;
			adlerB = (adlerB + adlerA) % 65521;
			if (++col == 1 + ICON_W * 4) {
				col = 0;
				row++;
			}
		}
	}
	put32(p, (adlerB << 16) | adlerA);
	p += 4;
	const unsigned idatLen = (unsigned)(p - idat - 8);
	put32(idat, idatLen);
	memcpy(idat + 4, "IDAT", 4);
	put32(p, crc32(idat + 4, idatLen + 4, 0));
	p += 4;
	// IEND
	put32(p, 0);
	memcpy(p + 4, "IEND", 4);
	put32(p + 8, crc32(p + 4, 4, 0));
	p += 12;
	return (unsigned)(p - out);
}

} // namespace bannericon

#endif
