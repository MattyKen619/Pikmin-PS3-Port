// PS3 port: GameCube texture decoding (see gpu_tex.h).
//
// The formats are stored in blocks (tiles) of 32 bytes (RGBA8: 64, two 32-byte halves), row by row of
// blocks: 8x8 texels for 4-bit formats, 8x4 for 8-bit formats, 4x4 for 16-bit formats. CMPR blocks are
// 8x8 texels made of four DXT1-like 4x4 sub-blocks (top left, top right, bottom left, bottom right).

#include "gpu_tex.h"

namespace gpu {

namespace {

inline u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
inline u32 c3to8(u32 v) { return (v << 5) | (v << 2) | (v >> 1); }
inline u32 c4to8(u32 v) { return (v << 4) | v; }
inline u32 c5to8(u32 v) { return (v << 3) | (v >> 2); }
inline u32 c6to8(u32 v) { return (v << 2) | (v >> 4); }

struct Packer {
	bool argb;
	inline u32 operator()(u32 r, u32 g, u32 b, u32 a) const
	{
		return argb ? (a << 24) | (r << 16) | (g << 8) | b : (r << 24) | (g << 16) | (b << 8) | a;
	}
};

inline u32 rgb565(const Packer& P, u32 v) { return P(c5to8((v >> 11) & 31), c6to8((v >> 5) & 63), c5to8(v & 31), 255); }
inline u32 rgb5a3(const Packer& P, u32 v)
{
	if (v & 0x8000) {
		return P(c5to8((v >> 10) & 31), c5to8((v >> 5) & 31), c5to8(v & 31), 255);
	}
	return P(c4to8((v >> 8) & 15), c4to8((v >> 4) & 15), c4to8(v & 15), c3to8((v >> 12) & 7));
}
inline u32 ia8(const Packer& P, u32 v) // first byte alpha, second intensity
{
	u32 a = v >> 8, i = v & 0xFF;
	return P(i, i, i, a);
}

} // namespace

void decodeTexture(const u8* src, u32 width, u32 height, u32 format, const u8* tmem, u32 tlut, u32 tlutFormat, u32* out,
                   u32 outPitch, TexelOrder order)
{
	Packer P;
	P.argb = order == TEXELS_ARGB;
	u32 bw, bh, bb;
	switch (format) {
	case TF_I4:
	case TF_C4:
	case TF_CMPR: bw = 8, bh = 8, bb = 32; break;
	case TF_I8:
	case TF_IA4:
	case TF_C8: bw = 8, bh = 4, bb = 32; break;
	case TF_RGBA8: bw = 4, bh = 4, bb = 64; break;
	default: bw = 4, bh = 4, bb = 32; break;
	}

	// palette (color-indexed formats): converted once
	u32 palette[256];
	const bool indexed = isIndexedFormat(format);
	if (indexed && format != TF_C14X2) {
		u32 n = paletteEntries(format);
		for (u32 i = 0; i < n; i++) {
			u32 v = be16(tmem + ((tlut + i * 2) & 0xFFFFF));
			palette[i] = tlutFormat == 0 ? ia8(P, v) : tlutFormat == 1 ? rgb565(P, v) : rgb5a3(P, v);
		}
	}

	const u32 blocksX = (width + bw - 1) / bw;
	const u32 blocksY = (height + bh - 1) / bh;
	for (u32 by = 0; by < blocksY; by++) {
		for (u32 bx = 0; bx < blocksX; bx++) {
			const u8* b = src + (by * blocksX + bx) * bb;
			const u32 x0 = bx * bw, y0 = by * bh;
			const bool partial = x0 + bw > width || y0 + bh > height;
			u32 block[64]; // bw x bh texels
			switch (format) {
			case TF_I4:
				for (u32 i = 0; i < 64; i++) {
					u32 v    = c4to8((b[i / 2] >> ((i & 1) ? 0 : 4)) & 0xF);
					block[i] = P(v, v, v, v);
				}
				break;
			case TF_I8:
				for (u32 i = 0; i < 32; i++) {
					block[i] = P(b[i], b[i], b[i], b[i]);
				}
				break;
			case TF_IA4:
				for (u32 i = 0; i < 32; i++) {
					u32 l = c4to8(b[i] & 0xF), a = c4to8(b[i] >> 4);
					block[i] = P(l, l, l, a);
				}
				break;
			case TF_IA8:
				for (u32 i = 0; i < 16; i++) {
					block[i] = ia8(P, be16(b + i * 2));
				}
				break;
			case TF_RGB565:
				for (u32 i = 0; i < 16; i++) {
					block[i] = rgb565(P, be16(b + i * 2));
				}
				break;
			case TF_RGB5A3:
				for (u32 i = 0; i < 16; i++) {
					block[i] = rgb5a3(P, be16(b + i * 2));
				}
				break;
			case TF_RGBA8:
				for (u32 i = 0; i < 16; i++) {
					block[i] = P(b[i * 2 + 1], b[32 + i * 2], b[32 + i * 2 + 1], b[i * 2]);
				}
				break;
			case TF_C4:
				for (u32 i = 0; i < 64; i++) {
					block[i] = palette[(b[i / 2] >> ((i & 1) ? 0 : 4)) & 0xF];
				}
				break;
			case TF_C8:
				for (u32 i = 0; i < 32; i++) {
					block[i] = palette[b[i]];
				}
				break;
			case TF_C14X2:
				for (u32 i = 0; i < 16; i++) {
					u32 v = be16(tmem + ((tlut + (be16(b + i * 2) & 0x3FFF) * 2) & 0xFFFFF));
					block[i] = tlutFormat == 0 ? ia8(P, v) : tlutFormat == 1 ? rgb565(P, v) : rgb5a3(P, v);
				}
				break;
			case TF_CMPR:
				for (u32 sub = 0; sub < 4; sub++) {
					const u8* s = b + sub * 8;
					u32 c0 = be16(s), c1 = be16(s + 2);
					u32 r0 = c5to8((c0 >> 11) & 31), g0 = c6to8((c0 >> 5) & 63), b0 = c5to8(c0 & 31);
					u32 r1 = c5to8((c1 >> 11) & 31), g1 = c6to8((c1 >> 5) & 63), b1 = c5to8(c1 & 31);
					u32 pal[4];
					pal[0] = P(r0, g0, b0, 255);
					pal[1] = P(r1, g1, b1, 255);
					if (c0 > c1) {
						pal[2] = P((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
						pal[3] = P((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
					} else {
						pal[2] = P((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
						pal[3] = P(0, 0, 0, 0);
					}
					const u32 ox = (sub & 1) * 4, oy = (sub >> 1) * 4;
					for (u32 sy = 0; sy < 4; sy++) {
						u32 row = s[4 + sy];
						for (u32 sx = 0; sx < 4; sx++) {
							block[(oy + sy) * 8 + ox + sx] = pal[(row >> (6 - sx * 2)) & 3];
						}
					}
				}
				break;
			default:
				for (u32 i = 0; i < 64; i++) {
					block[i] = P(255, 0, 255, 255);
				}
				break;
			}
			// store
			if (!partial) {
				for (u32 py = 0; py < bh; py++) {
					u32* dst       = out + (y0 + py) * outPitch + x0;
					const u32* row = block + py * bw;
					for (u32 px = 0; px < bw; px++) {
						dst[px] = row[px];
					}
				}
			} else {
				for (u32 py = 0; py < bh && y0 + py < height; py++) {
					u32* dst       = out + (y0 + py) * outPitch + x0;
					const u32* row = block + py * bw;
					for (u32 px = 0; px < bw && x0 + px < width; px++) {
						dst[px] = row[px];
					}
				}
			}
		}
	}
}

u64 hashMemory(const u8* p, u32 size)
{
	u64 h       = 0x9E3779B97F4A7C15ULL ^ size;
	const u64 k = 0xFF51AFD7ED558CCDULL;
	u32 i       = 0;
	if (((size_t)p & 7) == 0) {
		const u64* w = (const u64*)p;
		for (; i + 32 <= size; i += 32, w += 4) {
			h = (h ^ w[0]) * k;
			h = (h ^ w[1]) * k;
			h = (h ^ w[2]) * k;
			h = (h ^ w[3]) * k;
			h ^= h >> 29;
		}
	}
	for (; i < size; i++) {
		h = (h ^ p[i]) * k;
		h ^= h >> 29;
	}
	return h ^ (h >> 32);
}

} // namespace gpu
