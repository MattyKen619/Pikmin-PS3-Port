// PS3 port: the XMB icon made from the game's own disc banner (builds 54-55).
//
// The GameCube menu shows a 96 x 32 picture for each disc: the banner, in /opening.bnr ("BNR1" or "BNR2", then
// the picture at 0x20 in the RGB5A3 format, in 4 x 4 tiles). A package built without an icon of its own carries
// a stand-in icon marked as such (tools/ps3/mark_icon.py); on start, the game replaces it with this banner,
// scaled to the XMB's 320 x 176 and saved as a PNG (see encodePng). Every player's icon comes from their own
// disc, and the repository holds no game artwork.
//
// Plain C types only: used by the PS3 backend (ps3_backend.cpp) and a host test (tools/ps3/disc_test).

#ifndef PS3_BANNER_ICON_H
#define PS3_BANNER_ICON_H

#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace bannericon {

enum {
	BANNER_W = 96, BANNER_H = 32,
	BANNER_OFFSET = 0x20,                      // in opening.bnr
	BANNER_BYTES  = BANNER_W * BANNER_H * 2,   // RGB5A3
	ICON_W = 320, ICON_H = 176,
	RAW_BYTES = ICON_H * (1 + ICON_W * 4),     // PNG rows, each with its filter byte
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

// ---- PNG: 8-bit RGBA, rows filtered as encoders usually do, compressed (deflate: LZ77 and one dynamic Huffman
// block), the compressed data in 8 KB IDAT chunks as libpng writes them. (Build 54 wrote it uncompressed, about
// 220 KB in one chunk: a valid PNG, but the XMB showed its blank icon instead.)

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

static inline int paeth(int a, int b, int c)
{
	const int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
	return (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
}

// The rows with their filter byte, each row's filter the one with the smallest sum of |residual|.
static inline void filterRows(const unsigned char* rgba, unsigned char* raw)
{
	enum { ROW = ICON_W * 4, BPP = 4 };
	static const unsigned char zero[ROW] = { 0 };
	for (int y = 0; y < ICON_H; y++) {
		const unsigned char* cur  = rgba + y * ROW;
		const unsigned char* up   = y ? cur - ROW : zero;
		unsigned char* out        = raw + y * (ROW + 1);
		unsigned bestSum          = ~0u;
		int best                  = 0;
		for (int f = 0; f < 5; f++) {
			unsigned sum = 0;
			for (int i = 0; i < ROW; i++) {
				const int a = i >= BPP ? cur[i - BPP] : 0, b = up[i], c = i >= BPP ? up[i - BPP] : 0;
				const int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) >> 1 : paeth(a, b, c);
				const signed char r = (signed char)(unsigned char)(cur[i] - pred);
				sum += r < 0 ? -r : r;
			}
			if (sum < bestSum) {
				bestSum = sum;
				best    = f;
			}
		}
		out[0] = (unsigned char)best;
		for (int i = 0; i < ROW; i++) {
			const int a = i >= BPP ? cur[i - BPP] : 0, b = up[i], c = i >= BPP ? up[i - BPP] : 0;
			const int pred = best == 0 ? 0 : best == 1 ? a : best == 2 ? b : best == 3 ? (a + b) >> 1 : paeth(a, b, c);
			out[1 + i] = (unsigned char)(cur[i] - pred);
		}
	}
}

struct Bits {
	unsigned char* p;
	unsigned char* end;
	unsigned long long acc;
	int n;
};

static inline void putBits(Bits* b, unsigned value, int count) // least significant bit first
{
	b->acc |= (unsigned long long)value << b->n;
	b->n += count;
	while (b->n >= 8) {
		if (b->p < b->end) {
			*b->p = (unsigned char)b->acc;
		}
		b->p++;
		b->acc >>= 8;
		b->n -= 8;
	}
}

static inline void putCode(Bits* b, unsigned code, int len) // Huffman codes go most significant bit first
{
	unsigned r = 0;
	for (int i = 0; i < len; i++) {
		r = (r << 1) | ((code >> i) & 1);
	}
	putBits(b, r, len);
}

// Huffman code lengths for freq[0..n) (n <= 288), none longer than limit. Unused symbols get 0.
static inline void codeLengths(const unsigned* freq, int n, int limit, unsigned char* len)
{
	unsigned f[288];
	for (int i = 0; i < n; i++) {
		f[i] = freq[i];
	}
	for (;;) {
		unsigned weight[2 * 288];
		int parent[2 * 288];
		bool live[2 * 288];
		int nodes = 0, used = 0;
		for (int i = 0; i < n; i++) {
			weight[nodes] = f[i];
			parent[nodes] = -1;
			live[nodes]   = f[i] != 0;
			used += f[i] != 0;
			nodes++;
		}
		for (int i = 0; i < n; i++) {
			len[i] = 0;
		}
		if (used == 0) {
			return;
		}
		if (used == 1) {
			for (int i = 0; i < n; i++) {
				if (f[i]) {
					len[i] = 1;
				}
			}
			return;
		}
		for (int left = used; left > 1; left--) {
			int m1 = -1, m2 = -1;
			for (int i = 0; i < nodes; i++) {
				if (!live[i]) {
					continue;
				}
				if (m1 < 0 || weight[i] < weight[m1]) {
					m2 = m1;
					m1 = i;
				} else if (m2 < 0 || weight[i] < weight[m2]) {
					m2 = i;
				}
			}
			weight[nodes] = weight[m1] + weight[m2];
			parent[nodes] = -1;
			live[nodes]   = true;
			live[m1] = live[m2] = false;
			parent[m1] = parent[m2] = nodes;
			nodes++;
		}
		int longest = 0;
		for (int i = 0; i < n; i++) {
			if (!f[i]) {
				continue;
			}
			int depth = 0;
			for (int j = i; parent[j] >= 0; j = parent[j]) {
				depth++;
			}
			len[i]  = (unsigned char)depth;
			longest = depth > longest ? depth : longest;
		}
		if (longest <= limit) {
			return;
		}
		for (int i = 0; i < n; i++) { // too deep: flatten the frequencies and try again
			if (f[i]) {
				f[i] = (f[i] >> 1) | 1;
			}
		}
	}
}

// Canonical codes for the lengths (RFC 1951, 3.2.2).
static inline void canonicalCodes(const unsigned char* len, int n, unsigned* code)
{
	unsigned count[16] = { 0 }, next[16] = { 0 };
	for (int i = 0; i < n; i++) {
		count[len[i]]++;
	}
	count[0] = 0;
	unsigned c = 0;
	for (int bits = 1; bits < 16; bits++) {
		c          = (c + count[bits - 1]) << 1;
		next[bits] = c;
	}
	for (int i = 0; i < n; i++) {
		code[i] = len[i] ? next[len[i]]++ : 0;
	}
}

static const unsigned short kLenBase[29]  = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned char kLenExtra[29]  = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short kDistBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned char kDistExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static inline int lengthSymbol(int len)
{
	int i = 28;
	while (kLenBase[i] > len) {
		i--;
	}
	return i;
}

static inline int distanceSymbol(int dist)
{
	int i = 29;
	while (kDistBase[i] > dist) {
		i--;
	}
	return i;
}

// A zlib stream of in[0..n): LZ77 (hash chains) and one dynamic Huffman block. Returns its size, or 0 if it did
// not fit in cap bytes or there was no memory.
static inline unsigned deflateZlib(const unsigned char* in, unsigned n, unsigned char* out, unsigned cap)
{
	enum { HASH = 1 << 15, WINDOW = 32768, MAX_CHAIN = 64, MIN_MATCH = 3, MAX_MATCH = 258 };
	int* head             = (int*)malloc(HASH * sizeof(int));
	int* prev             = (int*)malloc(n * sizeof(int));
	unsigned short* tLen  = (unsigned short*)malloc(n * sizeof(unsigned short)); // literal byte, or match length
	unsigned short* tDist = (unsigned short*)malloc(n * sizeof(unsigned short)); // 0 = literal
	unsigned result       = 0;
	if (head && prev && tLen && tDist) {
		for (int i = 0; i < HASH; i++) {
			head[i] = -1;
		}
		unsigned tokens = 0;
		unsigned freqL[286] = { 0 }, freqD[30] = { 0 };
		for (unsigned i = 0; i < n;) {
			int best = 0, bestDist = 0;
			unsigned h = 0;
			if (i + 2 < n) {
				h = (((unsigned)in[i] << 10) ^ ((unsigned)in[i + 1] << 5) ^ in[i + 2]) & (HASH - 1);
				const int maxLen = (int)(n - i < MAX_MATCH ? n - i : MAX_MATCH);
				int chain = 0;
				for (int j = head[h]; j >= 0 && (int)i - j <= WINDOW && chain < MAX_CHAIN; j = prev[j], chain++) {
					if (in[j + best] != in[i + best]) {
						continue;
					}
					int l = 0;
					while (l < maxLen && in[j + l] == in[i + l]) {
						l++;
					}
					if (l > best) {
						best     = l;
						bestDist = (int)i - j;
						if (l == maxLen) {
							break;
						}
					}
				}
			}
			const unsigned step = best >= MIN_MATCH ? (unsigned)best : 1;
			if (best >= MIN_MATCH) {
				tLen[tokens]  = (unsigned short)best;
				tDist[tokens] = (unsigned short)bestDist;
				freqL[257 + lengthSymbol(best)]++;
				freqD[distanceSymbol(bestDist)]++;
			} else {
				tLen[tokens]  = in[i];
				tDist[tokens] = 0;
				freqL[in[i]]++;
			}
			tokens++;
			for (unsigned k = i; k < i + step; k++) {
				if (k + 2 < n) {
					const unsigned hk = (((unsigned)in[k] << 10) ^ ((unsigned)in[k + 1] << 5) ^ in[k + 2]) & (HASH - 1);
					prev[k]  = head[hk];
					head[hk] = (int)k;
				}
			}
			i += step;
		}
		freqL[256] = 1; // end of block
		unsigned usedD = 0;
		for (int i = 0; i < 30; i++) {
			usedD += freqD[i] != 0;
		}
		if (usedD < 2) { // (two distance codes at least, so that every decoder takes the code as complete)
			freqD[0] += 1;
			freqD[1] += 1;
		}
		unsigned char lenL[286], lenD[30];
		unsigned codeL[286], codeD[30];
		codeLengths(freqL, 286, 15, lenL);
		codeLengths(freqD, 30, 15, lenD);
		canonicalCodes(lenL, 286, codeL);
		canonicalCodes(lenD, 30, codeD);
		int hlit = 286, hdist = 30;
		while (hlit > 257 && !lenL[hlit - 1]) {
			hlit--;
		}
		while (hdist > 1 && !lenD[hdist - 1]) {
			hdist--;
		}
		// The code lengths, run-length coded (symbols 16: repeat the last 3-6 times, 17: 3-10 zeros, 18: 11-138).
		unsigned char all[286 + 30], clSym[286 + 30], clExtra[286 + 30];
		int total = 0, nCl = 0;
		for (int i = 0; i < hlit; i++) {
			all[total++] = lenL[i];
		}
		for (int i = 0; i < hdist; i++) {
			all[total++] = lenD[i];
		}
		for (int i = 0; i < total;) {
			const unsigned char v = all[i];
			int run               = 1;
			while (i + run < total && all[i + run] == v) {
				run++;
			}
			int r = run;
			if (v == 0) {
				while (r >= 11) {
					const int k = r < 138 ? r : 138;
					clSym[nCl] = 18, clExtra[nCl++] = (unsigned char)(k - 11);
					r -= k;
				}
				if (r >= 3) {
					clSym[nCl] = 17, clExtra[nCl++] = (unsigned char)(r - 3);
					r = 0;
				}
			} else {
				clSym[nCl] = v, clExtra[nCl++] = 0;
				r--;
				while (r >= 3) {
					const int k = r < 6 ? r : 6;
					clSym[nCl] = 16, clExtra[nCl++] = (unsigned char)(k - 3);
					r -= k;
				}
			}
			while (r-- > 0) {
				clSym[nCl] = v, clExtra[nCl++] = 0;
			}
			i += run;
		}
		unsigned freqC[19] = { 0 };
		for (int i = 0; i < nCl; i++) {
			freqC[clSym[i]]++;
		}
		unsigned usedC = 0;
		for (int i = 0; i < 19; i++) {
			usedC += freqC[i] != 0;
		}
		if (usedC < 2) {
			freqC[freqC[0] ? 1 : 0] += 1;
		}
		unsigned char lenC[19];
		unsigned codeC[19];
		codeLengths(freqC, 19, 7, lenC);
		canonicalCodes(lenC, 19, codeC);
		static const unsigned char order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
		int hclen = 19;
		while (hclen > 4 && !lenC[order[hclen - 1]]) {
			hclen--;
		}

		Bits b = { out, out + cap, 0, 0 };
		putBits(&b, 0x78, 8);
		putBits(&b, 0x9C, 8);
		putBits(&b, 1, 1); // last block
		putBits(&b, 2, 2); // dynamic Huffman
		putBits(&b, (unsigned)(hlit - 257), 5);
		putBits(&b, (unsigned)(hdist - 1), 5);
		putBits(&b, (unsigned)(hclen - 4), 4);
		for (int i = 0; i < hclen; i++) {
			putBits(&b, lenC[order[i]], 3);
		}
		for (int i = 0; i < nCl; i++) {
			putCode(&b, codeC[clSym[i]], lenC[clSym[i]]);
			if (clSym[i] == 16) {
				putBits(&b, clExtra[i], 2);
			} else if (clSym[i] == 17) {
				putBits(&b, clExtra[i], 3);
			} else if (clSym[i] == 18) {
				putBits(&b, clExtra[i], 7);
			}
		}
		for (unsigned t = 0; t < tokens; t++) {
			if (!tDist[t]) {
				putCode(&b, codeL[tLen[t]], lenL[tLen[t]]);
				continue;
			}
			const int ls = lengthSymbol(tLen[t]), ds = distanceSymbol(tDist[t]);
			putCode(&b, codeL[257 + ls], lenL[257 + ls]);
			putBits(&b, tLen[t] - kLenBase[ls], kLenExtra[ls]);
			putCode(&b, codeD[ds], lenD[ds]);
			putBits(&b, tDist[t] - kDistBase[ds], kDistExtra[ds]);
		}
		putCode(&b, codeL[256], lenL[256]);
		if (b.n) {
			putBits(&b, 0, 8 - b.n);
		}
		unsigned a = 1, c = 0;
		for (unsigned i = 0; i < n; i++) {
			a = (a + in[i]) % 65521;
			c = (c + a) % 65521;
		}
		putBits(&b, (c >> 8) & 255, 8);
		putBits(&b, c & 255, 8);
		putBits(&b, (a >> 8) & 255, 8);
		putBits(&b, a & 255, 8);
		if (b.p <= b.end) {
			result = (unsigned)(b.p - out);
		}
	}
	free(head);
	free(prev);
	free(tLen);
	free(tDist);
	return result;
}

// The same data in "stored" (uncompressed) blocks: the fallback if compressing fails.
static inline unsigned storedZlib(const unsigned char* in, unsigned n, unsigned char* out)
{
	unsigned char* p = out;
	*p++ = 0x78;
	*p++ = 0x01;
	unsigned a = 1, c = 0;
	for (unsigned done = 0; done < n || done == 0;) {
		const unsigned k = n - done < 65535 ? n - done : 65535;
		*p++ = done + k >= n ? 1 : 0;
		*p++ = (unsigned char)k;
		*p++ = (unsigned char)(k >> 8);
		*p++ = (unsigned char)~k;
		*p++ = (unsigned char)(~k >> 8);
		for (unsigned i = 0; i < k; i++) {
			*p++ = in[done + i];
			a    = (a + in[done + i]) % 65521;
			c    = (c + a) % 65521;
		}
		done += k;
		if (n == 0) {
			break;
		}
	}
	put32(p, (c << 16) | a);
	return (unsigned)(p + 4 - out);
}

enum {
	ZLIB_CAP  = 2 + RAW_BYTES + 5 * ((RAW_BYTES + 65534) / 65535) + 4, // stored: the most it can take
	IDAT_PART = 8192,
	PNG_BYTES = 8 + 25 + ZLIB_CAP + 12 * ((ZLIB_CAP + IDAT_PART - 1) / IDAT_PART) + 12,
};

// Writes the PNG of an ICON_W x ICON_H RGBA picture to out (PNG_BYTES). Returns its size, 0 without memory.
// *compressed: whether the data could be compressed (else it is stored).
static inline unsigned encodePng(const unsigned char* rgba, unsigned char* out, bool* compressed = 0)
{
	unsigned char* raw  = (unsigned char*)malloc(RAW_BYTES);
	unsigned char* zlib = (unsigned char*)malloc(ZLIB_CAP);
	if (!raw || !zlib) {
		free(raw);
		free(zlib);
		return 0;
	}
	filterRows(rgba, raw);
	unsigned zlen = deflateZlib(raw, RAW_BYTES, zlib, ZLIB_CAP);
	if (compressed) {
		*compressed = zlen != 0;
	}
	if (!zlen) {
		for (int y = 0; y < ICON_H; y++) { // (unfiltered: as plain as it gets)
			raw[y * (1 + ICON_W * 4)] = 0;
			memcpy(raw + y * (1 + ICON_W * 4) + 1, rgba + y * ICON_W * 4, ICON_W * 4);
		}
		zlen = storedZlib(raw, RAW_BYTES, zlib);
	}
	static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	unsigned char* p = out;
	memcpy(p, sig, 8);
	p += 8;
	put32(p, 13);
	memcpy(p + 4, "IHDR", 4);
	put32(p + 8, ICON_W);
	put32(p + 12, ICON_H);
	p[16] = 8; // bits per channel
	p[17] = 6; // RGBA
	p[18] = p[19] = p[20] = 0;
	put32(p + 21, crc32(p + 4, 17, 0));
	p += 25;
	for (unsigned done = 0; done < zlen;) {
		const unsigned k = zlen - done < IDAT_PART ? zlen - done : IDAT_PART;
		put32(p, k);
		memcpy(p + 4, "IDAT", 4);
		memcpy(p + 8, zlib + done, k);
		put32(p + 8 + k, crc32(p + 4, k + 4, 0));
		p += 12 + k;
		done += k;
	}
	put32(p, 0);
	memcpy(p + 4, "IEND", 4);
	put32(p + 8, crc32(p + 4, 4, 0));
	p += 12;
	free(raw);
	free(zlib);
	return (unsigned)(p - out);
}

} // namespace bannericon

#endif
