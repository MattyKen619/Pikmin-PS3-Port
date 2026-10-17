// PS3 port, host tools: software renderer for the GameCube's graphics processor (see sw_render.h).
//
// Register layouts follow the hardware as the GX library (src/gx) programs it.

#include "sw_render.h"

#include <math.h>
#include <string.h>
#include <zlib.h>

#include <stdio.h>

namespace swr {

static inline u32 bits(u32 v, u32 shift, u32 count) { return (v >> shift) & ((1u << count) - 1); }
static inline float asFloat(u32 w)
{
	union {
		u32 u;
		float f;
	} x;
	x.u = w;
	return x.f;
}
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }

// ================================================================== textures

namespace {

struct TexKey {
	u32 address, width, height, format, tlut, tlutFormat;
	bool operator<(const TexKey& o) const
	{
		if (address != o.address) return address < o.address;
		if (width != o.width) return width < o.width;
		if (height != o.height) return height < o.height;
		if (format != o.format) return format < o.format;
		if (tlut != o.tlut) return tlut < o.tlut;
		return tlutFormat < o.tlutFormat;
	}
};

struct Decoded {
	u32 width, height;
	std::vector<u32> rgba; // 0xRRGGBBAA
	u32 size;              // bytes in memory
};

std::map<TexKey, Decoded> sTexCache;

struct Bound {
	const u32* texels;
	u32 width, height;
	u32 wrapS, wrapT;
	bool linear;
	bool missing;
};
Bound sBound[8];

inline u32 pack(u32 r, u32 g, u32 b, u32 a) { return (r << 24) | (g << 16) | (b << 8) | a; }
inline u32 c4to8(u32 v) { return (v << 4) | v; }
inline u32 c5to8(u32 v) { return (v << 3) | (v >> 2); }
inline u32 c6to8(u32 v) { return (v << 2) | (v >> 4); }
inline u32 c3to8(u32 v) { return (v << 5) | (v << 2) | (v >> 1); }

u32 rgb565(u32 v) { return pack(c5to8(bits(v, 11, 5)), c6to8(bits(v, 5, 6)), c5to8(bits(v, 0, 5)), 255); }
u32 rgb5a3(u32 v)
{
	if (v & 0x8000) {
		return pack(c5to8(bits(v, 10, 5)), c5to8(bits(v, 5, 5)), c5to8(bits(v, 0, 5)), 255);
	}
	return pack(c4to8(bits(v, 8, 4)), c4to8(bits(v, 4, 4)), c4to8(bits(v, 0, 4)), c3to8(bits(v, 12, 3)));
}
u32 ia8(u32 v) // first byte alpha, second intensity
{
	u32 a = v >> 8, i = v & 0xFF;
	return pack(i, i, i, a);
}

u32 tlutColor(const u8* tmem, u32 tlut, u32 tlutFormat, u32 index)
{
	u32 v = be16(tmem + ((tlut + index * 2) & 0xFFFFF));
	switch (tlutFormat) {
	case 0: return ia8(v);
	case 1: return rgb565(v);
	default: return rgb5a3(v);
	}
}

// Decodes a whole texture into RGBA (32-bit words 0xRRGGBBAA).
void decodeTexture(const u8* src, u32 width, u32 height, u32 format, const u8* tmem, u32 tlut, u32 tlutFormat, u32* out)
{
	u32 bw, bh, bb;
	switch (format) {
	case gpu::TF_I4:
	case gpu::TF_C4:
	case gpu::TF_CMPR: bw = 8, bh = 8, bb = 32; break;
	case gpu::TF_I8:
	case gpu::TF_IA4:
	case gpu::TF_C8: bw = 8, bh = 4, bb = 32; break;
	case gpu::TF_RGBA8: bw = 4, bh = 4, bb = 64; break;
	default: bw = 4, bh = 4, bb = 32; break;
	}
	const u32 blocksX = (width + bw - 1) / bw;
	const u32 blocksY = (height + bh - 1) / bh;
	for (u32 by = 0; by < blocksY; by++) {
		for (u32 bx = 0; bx < blocksX; bx++) {
			const u8* b = src + (by * blocksX + bx) * bb;
			for (u32 py = 0; py < bh; py++) {
				for (u32 px = 0; px < bw; px++) {
					u32 x = bx * bw + px, y = by * bh + py;
					if (x >= width || y >= height) {
						continue;
					}
					u32 c = 0;
					u32 i   = py * bw + px;
					switch (format) {
					case gpu::TF_I4:
					{
						u32 v = (b[i / 2] >> ((i & 1) ? 0 : 4)) & 0xF;
						v     = c4to8(v);
						c     = pack(v, v, v, v);
						break;
					}
					case gpu::TF_I8:
					{
						u32 v = b[i];
						c     = pack(v, v, v, v);
						break;
					}
					case gpu::TF_IA4:
					{
						u32 v = b[i];
						u32 l = c4to8(v & 0xF), a = c4to8(v >> 4);
						c     = pack(l, l, l, a);
						break;
					}
					case gpu::TF_IA8: c = ia8(be16(b + i * 2)); break;
					case gpu::TF_RGB565: c = rgb565(be16(b + i * 2)); break;
					case gpu::TF_RGB5A3: c = rgb5a3(be16(b + i * 2)); break;
					case gpu::TF_RGBA8:
					{
						u32 a = b[i * 2], r = b[i * 2 + 1], g = b[32 + i * 2], bl = b[32 + i * 2 + 1];
						c     = pack(r, g, bl, a);
						break;
					}
					case gpu::TF_C4: c = tlutColor(tmem, tlut, tlutFormat, (b[i / 2] >> ((i & 1) ? 0 : 4)) & 0xF); break;
					case gpu::TF_C8: c = tlutColor(tmem, tlut, tlutFormat, b[i]); break;
					case gpu::TF_C14X2: c = tlutColor(tmem, tlut, tlutFormat, be16(b + i * 2) & 0x3FFF); break;
					case gpu::TF_CMPR:
					{
						// 4 DXT1-like sub-blocks of 4x4: top left, top right, bottom left, bottom right.
						const u8* s = b + ((py / 4) * 2 + (px / 4)) * 8;
						u32 c0 = be16(s), c1 = be16(s + 2);
						u32 sx = px & 3, sy = py & 3;
						u32 idx = (s[4 + sy] >> (6 - sx * 2)) & 3;
						u32 r0 = c5to8(bits(c0, 11, 5)), g0 = c6to8(bits(c0, 5, 6)), b0 = c5to8(bits(c0, 0, 5));
						u32 r1 = c5to8(bits(c1, 11, 5)), g1 = c6to8(bits(c1, 5, 6)), b1 = c5to8(bits(c1, 0, 5));
						switch (idx) {
						case 0: c = pack(r0, g0, b0, 255); break;
						case 1: c = pack(r1, g1, b1, 255); break;
						case 2:
							c = c0 > c1 ? pack((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255)
							            : pack((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
							break;
						default:
							c = c0 > c1 ? pack((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255) : pack(0, 0, 0, 0);
							break;
						}
						break;
					}
					default: c = pack(255, 0, 255, 255); break;
					}
					out[y * width + x] = c;
				}
			}
		}
	}
}

inline u32 wrapCoord(int v, u32 size, u32 mode)
{
	switch (mode) {
	case 0: return (u32)clampi(v, 0, (int)size - 1); // clamp
	case 2: // mirror
	{
		int period = (int)size * 2;
		int m      = ((v % period) + period) % period;
		return (u32)(m < (int)size ? m : period - 1 - m);
	}
	default: return (u32)(((v % (int)size) + (int)size) % (int)size); // repeat
	}
}

} // namespace

// ================================================================== renderer

Renderer::Renderer(gpu::CommandProcessor& cp, gpu::Memory& memory)
    : mCp(cp)
    , mMem(memory)
    , mFrameW(0)
    , mFrameH(0)
    , mFrames(0)
    , mSkipFirst(0)
    , mSkipLast(0)
    , mDrawNumber(0)
{
	memset(mColor, 0, sizeof(mColor));
	for (u32 i = 0; i < EFB_WIDTH * EFB_HEIGHT; i++) {
		mColor[i] = 0x000000FF;
		mDepth[i] = 0xFFFFFF;
	}
	memset(mTmem, 0, sizeof(mTmem));
	memset(mTevReg, 0, sizeof(mTevReg));
	memset(mKonst, 0, sizeof(mKonst));
	memset(&mStats, 0, sizeof(mStats));
	sTexCache.clear();
}

static void tevRegFromBp(int out[4][4], int other[4][4], u32 reg, u32 value)
{
	// BP 0xE0 + 2n: A (bits 12-22) and R (bits 0-10); 0xE1 + 2n: G (12-22) and B (0-10); bit 23: konst
	int n      = (reg - 0xE0) >> 1;
	bool konst = (value >> 23) & 1;
	int (*dst)[4] = konst ? other : out;
	int lo        = (int)(value & 0x7FF);
	int hi        = (int)((value >> 12) & 0x7FF);
	if (lo & 0x400) lo -= 0x800;
	if (hi & 0x400) hi -= 0x800;
	if ((reg & 1) == 0) {
		dst[n][0] = lo; // R
		dst[n][3] = hi; // A
	} else {
		dst[n][2] = lo; // B
		dst[n][1] = hi; // G
	}
}

void Renderer::begin()
{
	const gpu::State& S = mCp.state();
	for (u32 r = 0xE0; r < 0xE8; r++) {
		tevRegFromBp(mTevReg, mKonst, r, S.bp[r]);
	}
}

void Renderer::bpWritten(u32 reg, u32 value)
{
	if (reg >= 0xE0 && reg < 0xE8) {
		tevRegFromBp(mTevReg, mKonst, reg, value);
	} else if (reg == 0x65) {
		// TLUT load: from memory (BP 0x64) into texture memory
		const gpu::State& S = mCp.state();
		u32 address         = bits(S.bp[0x64], 0, 21) << 5;
		u32 tmem            = bits(value, 0, 10) << 9;
		u32 bytes           = bits(value, 10, 11) * 32;
		const u8* src       = bytes ? mMem.read(address, bytes, gpu::MEM_TLUT) : NULL;
		if (src && tmem + bytes <= sizeof(mTmem)) {
			memcpy(mTmem + tmem, src, bytes);
			sTexCache.clear(); // (palettes changed)
		}
	}
}

// ---------------------------------------------------------------- vertices

void Renderer::transform(const gpu::Vertex& in, const gpu::VertexLayout& L, OutVertex& out)
{
	const u32* xf = mCp.state().xf;
	const u32 idx = in.pnmtx & 63;
	const u32* m  = xf + idx * 4;
	float pos[3]  = { in.pos[0], in.pos[1], L.posCount == 3 ? in.pos[2] : 0.0f };
	float view[3];
	for (int r = 0; r < 3; r++) {
		view[r] = asFloat(m[r * 4]) * pos[0] + asFloat(m[r * 4 + 1]) * pos[1] + asFloat(m[r * 4 + 2]) * pos[2] + asFloat(m[r * 4 + 3]);
	}
	// normal
	float nrm[3] = { 0, 0, 1 };
	if (L.nrmMode != gpu::ATTR_NONE) {
		const u32* n = xf + 0x400 + (idx & 31) * 3;
		for (int r = 0; r < 3; r++) {
			nrm[r] = asFloat(n[r * 3]) * in.nrm[0] + asFloat(n[r * 3 + 1]) * in.nrm[1] + asFloat(n[r * 3 + 2]) * in.nrm[2];
		}
		float len = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
		if (len > 0) {
			nrm[0] /= len, nrm[1] /= len, nrm[2] /= len;
		}
	}
	// projection
	float p[6];
	for (int i = 0; i < 6; i++) {
		p[i] = asFloat(xf[0x1020 + i]);
	}
	if (xf[0x1026] == 0) { // perspective
		out.clip[0] = p[0] * view[0] + p[1] * view[2];
		out.clip[1] = p[2] * view[1] + p[3] * view[2];
		out.clip[2] = p[4] * view[2] + p[5];
		out.clip[3] = -view[2];
	} else {
		out.clip[0] = p[0] * view[0] + p[1];
		out.clip[1] = p[2] * view[1] + p[3];
		out.clip[2] = p[4] * view[2] + p[5];
		out.clip[3] = 1.0f;
	}

	// colors and lighting
	const u32 numChans = xf[0x1009] & 3;
	for (int c = 0; c < 2; c++) {
		float vtx[4] = { 1, 1, 1, 1 };
		if (L.clrMode[c] != gpu::ATTR_NONE) {
			for (int k = 0; k < 4; k++) {
				vtx[k] = in.clr[c][k] / 255.0f;
			}
		} else if (c == 1 && L.clrMode[0] != gpu::ATTR_NONE) {
			for (int k = 0; k < 4; k++) {
				vtx[k] = in.clr[0][k] / 255.0f;
			}
		}
		if ((u32)c >= numChans) {
			for (int k = 0; k < 4; k++) {
				out.color[c][k] = 0;
			}
			continue;
		}
		for (int part = 0; part < 2; part++) { // 0: color (rgb), 1: alpha
			const u32 ctrl = xf[(part ? 0x1010 : 0x100E) + c];
			const u32 matR = xf[0x100C + c], ambR = xf[0x100A + c];
			float mat[4], amb[4];
			for (int k = 0; k < 4; k++) {
				mat[k] = bits(ctrl, 0, 1) ? vtx[k] : ((matR >> (24 - k * 8)) & 0xFF) / 255.0f;
				amb[k] = bits(ctrl, 6, 1) ? vtx[k] : ((ambR >> (24 - k * 8)) & 0xFF) / 255.0f;
			}
			float result[4];
			if (bits(ctrl, 1, 1)) {
				float acc[4] = { amb[0], amb[1], amb[2], amb[3] };
				const u32 mask = bits(ctrl, 2, 4) | (bits(ctrl, 11, 4) << 4);
				const u32 diffuse = bits(ctrl, 7, 2);
				const bool attnOn = bits(ctrl, 9, 1), spot = bits(ctrl, 10, 1);
				for (int l = 0; l < 8; l++) {
					if (!(mask & (1u << l))) {
						continue;
					}
					const u32* lt = xf + 0x600 + l * 16;
					float lcol[4] = { ((lt[3] >> 24) & 0xFF) / 255.0f, ((lt[3] >> 16) & 0xFF) / 255.0f, ((lt[3] >> 8) & 0xFF) / 255.0f,
						              (lt[3] & 0xFF) / 255.0f };
					float A[3] = { asFloat(lt[4]), asFloat(lt[5]), asFloat(lt[6]) };
					float K[3] = { asFloat(lt[7]), asFloat(lt[8]), asFloat(lt[9]) };
					float lp[3] = { asFloat(lt[10]), asFloat(lt[11]), asFloat(lt[12]) };
					float ld[3] = { asFloat(lt[13]), asFloat(lt[14]), asFloat(lt[15]) };
					float dir[3], attn = 1.0f;
					if (attnOn && !spot) { // specular
						float len = sqrtf(lp[0] * lp[0] + lp[1] * lp[1] + lp[2] * lp[2]);
						for (int k = 0; k < 3; k++) dir[k] = len > 0 ? lp[k] / len : 0;
						float nd  = nrm[0] * dir[0] + nrm[1] * dir[1] + nrm[2] * dir[2];
						float a   = nd >= 0 ? fmaxf(0, nrm[0] * ld[0] + nrm[1] * ld[1] + nrm[2] * ld[2]) : 0;
						float cosA = A[0] + A[1] * a + A[2] * a * a;
						float disA = K[0] + K[1] * a + K[2] * a * a;
						attn       = disA != 0 ? fmaxf(0, cosA) / disA : 0;
					} else {
						for (int k = 0; k < 3; k++) dir[k] = lp[k] - view[k];
						float d2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
						float d  = sqrtf(d2);
						for (int k = 0; k < 3; k++) dir[k] = d > 0 ? dir[k] / d : 0;
						if (attnOn) {
							float a    = fmaxf(0, dir[0] * ld[0] + dir[1] * ld[1] + dir[2] * ld[2]);
							float cosA = A[0] + A[1] * a + A[2] * a * a;
							float disA = K[0] + K[1] * d + K[2] * d2;
							attn       = disA != 0 ? fmaxf(0, cosA) / disA : 0;
						}
					}
					float dif = 1.0f;
					if (diffuse) {
						dif = nrm[0] * dir[0] + nrm[1] * dir[1] + nrm[2] * dir[2];
						if (diffuse == 2) dif = fmaxf(0, dif);
					}
					for (int k = 0; k < 4; k++) acc[k] += attn * dif * lcol[k];
				}
				for (int k = 0; k < 4; k++) result[k] = mat[k] * clampf(acc[k], 0, 1);
			} else {
				for (int k = 0; k < 4; k++) result[k] = mat[k];
			}
			if (part == 0) {
				out.color[c][0] = result[0], out.color[c][1] = result[1], out.color[c][2] = result[2];
			} else {
				out.color[c][3] = result[3];
			}
		}
	}

	// texture coordinates
	const u32 numTexGens = xf[0x103F] & 0xF;
	const bool dual      = xf[0x1012] & 1;
	for (u32 t = 0; t < 8; t++) {
		out.tex[t][0] = out.tex[t][1] = 0;
		out.tex[t][2] = 1;
		if (t >= numTexGens) {
			continue;
		}
		const u32 reg  = xf[0x1040 + t];
		const bool stq = bits(reg, 1, 1);
		const bool abc1 = bits(reg, 2, 1);
		const u32 type = bits(reg, 4, 3);
		const u32 row  = bits(reg, 7, 5);
		float src[4]   = { 0, 0, 1, 1 };
		switch (row) {
		case 0: src[0] = pos[0], src[1] = pos[1], src[2] = pos[2]; break;
		case 1: src[0] = in.nrm[0], src[1] = in.nrm[1], src[2] = in.nrm[2]; break;
		case 3: src[0] = in.tangent[0], src[1] = in.tangent[1], src[2] = in.tangent[2]; break;
		case 4: src[0] = in.binormal[0], src[1] = in.binormal[1], src[2] = in.binormal[2]; break;
		default:
			if (row >= 5 && row <= 12) {
				src[0] = in.tex[row - 5][0], src[1] = in.tex[row - 5][1];
			}
			break;
		}
		if (!abc1) {
			src[2] = 1.0f;
		}
		float s = 0, tt = 0, q = 1;
		if (type == 0) {
			const u32* tm = xf + (in.texmtx[t] & 63) * 4;
			s  = asFloat(tm[0]) * src[0] + asFloat(tm[1]) * src[1] + asFloat(tm[2]) * src[2] + asFloat(tm[3]) * src[3];
			tt = asFloat(tm[4]) * src[0] + asFloat(tm[5]) * src[1] + asFloat(tm[6]) * src[2] + asFloat(tm[7]) * src[3];
			if (stq) {
				q = asFloat(tm[8]) * src[0] + asFloat(tm[9]) * src[1] + asFloat(tm[10]) * src[2] + asFloat(tm[11]) * src[3];
			}
		} else if (type == 1) {
			mStats.embossTexgens++;
			u32 from = bits(reg, 12, 3);
			s = out.tex[from][0], tt = out.tex[from][1], q = out.tex[from][2];
		} else { // color: s = red, t = green of the channel
			int c = type == 2 ? 0 : 1;
			s = out.color[c][0], tt = out.color[c][1];
		}
		if (dual && type == 0) {
			const u32 post = xf[0x1050 + t];
			float v[3]     = { s, tt, q };
			if (bits(post, 8, 1)) {
				float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
				if (len > 0) v[0] /= len, v[1] /= len, v[2] /= len;
			}
			const u32* pm = xf + 0x500 + bits(post, 0, 6) * 4;
			s  = asFloat(pm[0]) * v[0] + asFloat(pm[1]) * v[1] + asFloat(pm[2]) * v[2] + asFloat(pm[3]);
			tt = asFloat(pm[4]) * v[0] + asFloat(pm[5]) * v[1] + asFloat(pm[6]) * v[2] + asFloat(pm[7]);
			q  = asFloat(pm[8]) * v[0] + asFloat(pm[9]) * v[1] + asFloat(pm[10]) * v[2] + asFloat(pm[11]);
		}
		out.tex[t][0] = s, out.tex[t][1] = tt, out.tex[t][2] = q;
	}
}

// ---------------------------------------------------------------- textures for a draw

static void bindTextures(const gpu::State& S, gpu::Memory& mem, const u8* tmem, const std::map<u32, CopyTexture>& copies, Stats& st)
{
	const u32 genmode = S.bp[0];
	const u32 stages  = bits(genmode, 10, 4) + 1;
	u32 used          = 0;
	for (u32 s = 0; s < stages; s++) {
		u32 tref = S.bp[0x28 + s / 2] >> ((s & 1) * 12);
		if (tref & 0x40) {
			used |= 1u << (tref & 7);
		}
	}
	for (u32 m = 0; m < 8; m++) {
		Bound& b  = sBound[m];
		b.missing = true;
		b.texels  = NULL;
		if (!(used & (1u << m))) {
			continue;
		}
		const u32 base = m < 4 ? m : m + 0x1C; // 0x80+m or 0xA0+(m-4)
		const u32 mode0 = S.bp[0x80 + base];
		const u32 img0  = S.bp[0x88 + base];
		const u32 img3  = S.bp[0x94 + base];
		const u32 tlutR = S.bp[0x98 + base];
		b.wrapS  = bits(mode0, 0, 2);
		b.wrapT  = bits(mode0, 2, 2);
		b.linear = bits(mode0, 4, 1);
		TexKey k;
		k.address    = bits(img3, 0, 21) << 5;
		k.width      = bits(img0, 0, 10) + 1;
		k.height     = bits(img0, 10, 10) + 1;
		k.format     = bits(img0, 20, 4);
		bool ci      = k.format == gpu::TF_C4 || k.format == gpu::TF_C8 || k.format == gpu::TF_C14X2;
		k.tlut       = ci ? bits(tlutR, 0, 10) << 9 : 0;
		k.tlutFormat = ci ? bits(tlutR, 10, 2) : 0;

		std::map<u32, CopyTexture>::const_iterator ct = copies.find(k.address);
		if (ct != copies.end()) {
			st.copyTextureUses++;
			b.texels  = ct->second.rgba.data();
			b.width   = ct->second.width;
			b.height  = ct->second.height;
			b.missing = false;
			continue;
		}
		std::map<TexKey, Decoded>::iterator it = sTexCache.find(k);
		if (it == sTexCache.end()) {
			u32 size      = gpu::CommandProcessor::textureSize(k.format, k.width, k.height, 1);
			const u8* src = mem.read(k.address, size, gpu::MEM_TEXTURE);
			if (!src) {
				st.missingTextures++;
				continue;
			}
			Decoded d;
			d.width  = k.width;
			d.height = k.height;
			d.size   = size;
			d.rgba.assign(k.width * k.height, 0);
			decodeTexture(src, k.width, k.height, k.format, tmem, k.tlut, k.tlutFormat, d.rgba.data());
			it = sTexCache.insert(std::make_pair(k, d)).first;
		}
		b.texels  = it->second.rgba.data();
		b.width   = it->second.width;
		b.height  = it->second.height;
		b.missing = false;
	}
}

void invalidateTextures(u32 address, u32 size)
{
	for (std::map<TexKey, Decoded>::iterator it = sTexCache.begin(); it != sTexCache.end();) {
		u32 a = it->first.address;
		if (a < address + size && address < a + it->second.size) {
			sTexCache.erase(it++);
		} else {
			++it;
		}
	}
}

void Renderer::sample(int map, float s, float t, float out[4])
{
	const Bound& b = sBound[map];
	if (b.missing) {
		out[0] = out[2] = 255, out[1] = 0, out[3] = 255; // magenta: texture not found
		return;
	}
	float u = s * b.width, v = t * b.height;
	if (!b.linear) {
		u32 x = wrapCoord((int)floorf(u), b.width, b.wrapS);
		u32 y = wrapCoord((int)floorf(v), b.height, b.wrapT);
		u32 c = b.texels[y * b.width + x];
		out[0] = (float)(c >> 24), out[1] = (float)((c >> 16) & 0xFF), out[2] = (float)((c >> 8) & 0xFF), out[3] = (float)(c & 0xFF);
		return;
	}
	u -= 0.5f, v -= 0.5f;
	int x0 = (int)floorf(u), y0 = (int)floorf(v);
	float fx = u - x0, fy = v - y0;
	u32 xs[2] = { wrapCoord(x0, b.width, b.wrapS), wrapCoord(x0 + 1, b.width, b.wrapS) };
	u32 ys[2] = { wrapCoord(y0, b.height, b.wrapT), wrapCoord(y0 + 1, b.height, b.wrapT) };
	for (int k = 0; k < 4; k++) {
		out[k] = 0;
	}
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
			u32 c   = b.texels[ys[j] * b.width + xs[i]];
			out[0] += w * (c >> 24), out[1] += w * ((c >> 16) & 0xFF), out[2] += w * ((c >> 8) & 0xFF), out[3] += w * (c & 0xFF);
		}
	}
}

// ---------------------------------------------------------------- draws

void Renderer::draw(const gpu::DrawCall& call)
{
	mDrawNumber++;
	mStats.draws++;
	if (mDrawNumber >= mSkipFirst && mDrawNumber <= mSkipLast && mSkipFirst) {
		return;
	}
	if (!call.vertices || !call.count) {
		return;
	}
	const gpu::State& S = mCp.state();
	const u32 prim      = call.primitive;
	if (prim == gpu::PRIM_LINES || prim == gpu::PRIM_LINE_STRIP) {
		mStats.lines++;
		return;
	}
	if (prim == gpu::PRIM_POINTS) {
		mStats.points++;
		return;
	}
	if (bits(S.bp[0xEE], 21, 3) != 0 || bits(S.bp[0xF2], 21, 3) != 0) {
		mStats.fogDraws++; // (fog type in BP 0xF2 bits 21-23)
	}
	if (bits(S.bp[0], 16, 3) != 0) {
		mStats.indirectDraws++;
	}
	if (S.xf[0x1012] & 1) {
		mStats.dualTexDraws++;
	}
	mCull = (int)bits(S.bp[0], 14, 2);
	mScissor[1] = (int)bits(S.bp[0x20], 0, 11) - 342;
	mScissor[0] = (int)bits(S.bp[0x20], 12, 11) - 342;
	mScissor[3] = (int)bits(S.bp[0x21], 0, 11) - 342;
	mScissor[2] = (int)bits(S.bp[0x21], 12, 11) - 342;
	bindTextures(S, mMem, mTmem, mCopies, mStats);

	std::vector<OutVertex> v(call.count);
	for (u32 i = 0; i < call.count; i++) {
		transform(call.vertices[i], *call.layout, v[i]);
	}
	const u32 n = call.count;
	switch (prim) {
	case gpu::PRIM_QUADS:
	case gpu::PRIM_QUADS_2:
		for (u32 i = 0; i + 3 < n; i += 4) {
			triangle(v[i], v[i + 1], v[i + 2]);
			triangle(v[i], v[i + 2], v[i + 3]);
		}
		break;
	case gpu::PRIM_TRIANGLES:
		for (u32 i = 0; i + 2 < n; i += 3) {
			triangle(v[i], v[i + 1], v[i + 2]);
		}
		break;
	case gpu::PRIM_TRI_STRIP:
		for (u32 i = 0; i + 2 < n; i++) {
			if (i & 1) {
				triangle(v[i + 1], v[i], v[i + 2]);
			} else {
				triangle(v[i], v[i + 1], v[i + 2]);
			}
		}
		break;
	case gpu::PRIM_TRI_FAN:
		for (u32 i = 1; i + 1 < n; i++) {
			triangle(v[0], v[i], v[i + 1]);
		}
		break;
	default: break;
	}
}

static void lerpVertex(const Renderer::OutVertex& a, const Renderer::OutVertex& b, float t, Renderer::OutVertex& out)
{
	const float* pa = (const float*)&a;
	const float* pb = (const float*)&b;
	float* po       = (float*)&out;
	for (u32 i = 0; i < sizeof(Renderer::OutVertex) / sizeof(float); i++) {
		po[i] = pa[i] + (pb[i] - pa[i]) * t;
	}
}

void Renderer::triangle(const OutVertex& a, const OutVertex& b, const OutVertex& c)
{
	mStats.triangles++;
	// Clip against the near plane (z >= -w) and w > 0.
	OutVertex in[8], out[8];
	int nin = 3;
	in[0] = a, in[1] = b, in[2] = c;
	for (int plane = 0; plane < 2; plane++) {
		int nout = 0;
		for (int i = 0; i < nin; i++) {
			const OutVertex& p = in[i];
			const OutVertex& q = in[(i + 1) % nin];
			float dp = plane == 0 ? p.clip[2] + p.clip[3] : p.clip[3] - 1e-5f;
			float dq = plane == 0 ? q.clip[2] + q.clip[3] : q.clip[3] - 1e-5f;
			if (dp >= 0) {
				out[nout++] = p;
			}
			if ((dp >= 0) != (dq >= 0)) {
				lerpVertex(p, q, dp / (dp - dq), out[nout++]);
			}
		}
		nin = nout;
		for (int i = 0; i < nin; i++) {
			in[i] = out[i];
		}
		if (nin < 3) {
			mStats.clipped++;
			return;
		}
	}
	for (int i = 1; i + 1 < nin; i++) {
		const OutVertex* tri[3] = { &in[0], &in[i], &in[i + 1] };
		rasterize(tri);
	}
}

namespace {
struct Screen {
	float x, y, z, invW;
};
} // namespace

void Renderer::rasterize(const OutVertex* v[3])
{
	const u32* xf = mCp.state().xf;
	const float xs = asFloat(xf[0x101A]), ys = asFloat(xf[0x101B]), zs = asFloat(xf[0x101C]);
	const float xo = asFloat(xf[0x101D]) - 342, yo = asFloat(xf[0x101E]) - 342, zo = asFloat(xf[0x101F]);
	Screen p[3];
	for (int i = 0; i < 3; i++) {
		float iw  = 1.0f / v[i]->clip[3];
		p[i].x    = xo + xs * v[i]->clip[0] * iw;
		p[i].y    = yo + ys * v[i]->clip[1] * iw;
		p[i].z    = zo + zs * v[i]->clip[2] * iw;
		p[i].invW = iw;
	}
	float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
	if (area == 0) {
		return;
	}
	// Cull (hardware mode: 1 = the GX library's GX_CULL_BACK, 2 = GX_CULL_FRONT, 3 = all)
	if (mCull == 3 || (mCull == 1 && area < 0) || (mCull == 2 && area > 0)) {
		mStats.culled++;
		return;
	}
	int minX = (int)floorf(fminf(p[0].x, fminf(p[1].x, p[2].x)));
	int maxX = (int)ceilf(fmaxf(p[0].x, fmaxf(p[1].x, p[2].x)));
	int minY = (int)floorf(fminf(p[0].y, fminf(p[1].y, p[2].y)));
	int maxY = (int)ceilf(fmaxf(p[0].y, fmaxf(p[1].y, p[2].y)));
	minX = clampi(minX, clampi(mScissor[0], 0, EFB_WIDTH - 1), EFB_WIDTH - 1);
	maxX = clampi(maxX, 0, clampi(mScissor[2], 0, EFB_WIDTH - 1));
	minY = clampi(minY, clampi(mScissor[1], 0, EFB_HEIGHT - 1), EFB_HEIGHT - 1);
	maxY = clampi(maxY, 0, clampi(mScissor[3], 0, EFB_HEIGHT - 1));
	if (minX > maxX || minY > maxY) {
		return;
	}
	const float inv = 1.0f / area;
	float color0[4], color1[4], tex[8][3];
	for (int y = minY; y <= maxY; y++) {
		for (int x = minX; x <= maxX; x++) {
			float px = x + 0.5f, py = y + 0.5f;
			float w0 = ((p[1].x - px) * (p[2].y - py) - (p[2].x - px) * (p[1].y - py)) * inv;
			float w1 = ((p[2].x - px) * (p[0].y - py) - (p[0].x - px) * (p[2].y - py)) * inv;
			float w2 = 1.0f - w0 - w1;
			if (w0 < 0 || w1 < 0 || w2 < 0) {
				continue;
			}
			float z = w0 * p[0].z + w1 * p[1].z + w2 * p[2].z;
			// perspective-correct attributes
			float pw0 = w0 * p[0].invW, pw1 = w1 * p[1].invW, pw2 = w2 * p[2].invW;
			float sum = pw0 + pw1 + pw2;
			pw0 /= sum, pw1 /= sum, pw2 /= sum;
			for (int k = 0; k < 4; k++) {
				color0[k] = pw0 * v[0]->color[0][k] + pw1 * v[1]->color[0][k] + pw2 * v[2]->color[0][k];
				color1[k] = pw0 * v[0]->color[1][k] + pw1 * v[1]->color[1][k] + pw2 * v[2]->color[1][k];
			}
			for (int t = 0; t < 8; t++) {
				for (int k = 0; k < 3; k++) {
					tex[t][k] = pw0 * v[0]->tex[t][k] + pw1 * v[1]->tex[t][k] + pw2 * v[2]->tex[t][k];
				}
			}
			shade(x, y, z, color0, color1, tex);
		}
	}
}

// ---------------------------------------------------------------- pixels

static bool compare(u32 func, int a, int b)
{
	switch (func) {
	case 0: return false;
	case 1: return a < b;
	case 2: return a == b;
	case 3: return a <= b;
	case 4: return a > b;
	case 5: return a != b;
	case 6: return a >= b;
	default: return true;
	}
}

void Renderer::shade(int x, int y, float zf, const float* color0, const float* color1, const float (*tex)[3])
{
	const gpu::State& S = mCp.state();
	mStats.pixels++;
	const u32 genmode = S.bp[0];
	const u32 stages  = bits(genmode, 10, 4) + 1;
	const u32 pe      = S.bp[0x43];
	const u32 zmode   = S.bp[0x40];
	const int zi      = clampi((int)(zf + 0.5f), 0, 0xFFFFFF);
	const u32 at      = y * EFB_WIDTH + x;
	const bool early  = bits(pe, 6, 1);
	if (early && bits(zmode, 0, 1) && !compare(bits(zmode, 1, 3), zi, (int)mDepth[at])) {
		return;
	}

	int regs[4][4];
	memcpy(regs, mTevReg, sizeof(regs));
	int ras[2][4];
	for (int k = 0; k < 4; k++) {
		ras[0][k] = clampi((int)(color0[k] * 255.0f + 0.5f), 0, 255);
		ras[1][k] = clampi((int)(color1[k] * 255.0f + 0.5f), 0, 255);
	}
	for (u32 s = 0; s < stages; s++) {
		const u32 tref = S.bp[0x28 + s / 2] >> ((s & 1) * 12);
		const u32 cenv = S.bp[0xC0 + s * 2];
		const u32 aenv = S.bp[0xC1 + s * 2];
		const u32 ksel = S.bp[0xF6 + s / 2];
		// swap tables: table i is in KSEL 2i (red bits 0-1, green 2-3) and 2i+1 (blue 0-1, alpha 2-3)
		int swapR[4], swapT[4];
		u32 rs = bits(aenv, 0, 2), ts = bits(aenv, 2, 2);
		swapR[0] = bits(S.bp[0xF6 + rs * 2], 0, 2), swapR[1] = bits(S.bp[0xF6 + rs * 2], 2, 2);
		swapR[2] = bits(S.bp[0xF7 + rs * 2], 0, 2), swapR[3] = bits(S.bp[0xF7 + rs * 2], 2, 2);
		swapT[0] = bits(S.bp[0xF6 + ts * 2], 0, 2), swapT[1] = bits(S.bp[0xF6 + ts * 2], 2, 2);
		swapT[2] = bits(S.bp[0xF7 + ts * 2], 0, 2), swapT[3] = bits(S.bp[0xF7 + ts * 2], 2, 2);

		int texc[4] = { 255, 255, 255, 255 };
		if (bits(tref, 6, 1)) {
			u32 map = bits(tref, 0, 3), coord = bits(tref, 3, 3);
			float q = tex[coord][2];
			if (q == 0) q = 1;
			float c[4];
			sample((int)map, tex[coord][0] / q, tex[coord][1] / q, c);
			int raw[4] = { (int)(c[0] + 0.5f), (int)(c[1] + 0.5f), (int)(c[2] + 0.5f), (int)(c[3] + 0.5f) };
			for (int k = 0; k < 4; k++) texc[k] = raw[swapT[k]];
		}
		int rasc[4] = { 0, 0, 0, 0 };
		u32 chan    = bits(tref, 7, 3);
		if (chan == 0 || chan == 1) {
			for (int k = 0; k < 4; k++) rasc[k] = ras[chan][swapR[k]];
		}
		// konst
		u32 kc = bits(ksel, (s & 1) ? 14 : 4, 5);
		u32 ka = bits(ksel, (s & 1) ? 19 : 9, 5);
		int konstC[3], konstA;
		static const int kFixed[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };
		if (kc < 8) {
			konstC[0] = konstC[1] = konstC[2] = kFixed[kc];
		} else if (kc >= 12 && kc < 16) {
			for (int k = 0; k < 3; k++) konstC[k] = mKonst[kc - 12][k];
		} else if (kc >= 16) {
			int comp = (kc - 16) / 4, reg = (kc - 16) % 4;
			konstC[0] = konstC[1] = konstC[2] = mKonst[reg][comp];
		} else {
			konstC[0] = konstC[1] = konstC[2] = 0;
		}
		if (ka < 8) {
			konstA = kFixed[ka];
		} else if (ka >= 16) {
			konstA = mKonst[(ka - 16) % 4][(ka - 16) / 4];
		} else {
			konstA = 0;
		}

		// color combiner
		int cin[16][3];
		for (int k = 0; k < 3; k++) {
			cin[0][k] = regs[0][k], cin[1][k] = regs[0][3];
			cin[2][k] = regs[1][k], cin[3][k] = regs[1][3];
			cin[4][k] = regs[2][k], cin[5][k] = regs[2][3];
			cin[6][k] = regs[3][k], cin[7][k] = regs[3][3];
			cin[8][k] = texc[k], cin[9][k] = texc[3];
			cin[10][k] = rasc[k], cin[11][k] = rasc[3];
			cin[12][k] = 255, cin[13][k] = 128;
			cin[14][k] = konstC[k], cin[15][k] = 0;
		}
		int ain[8] = { regs[0][3], regs[1][3], regs[2][3], regs[3][3], texc[3], rasc[3], konstA, 0 };

		const u32 cdest = bits(cenv, 22, 2), adest = bits(aenv, 22, 2);
		int outC[3], outA;
		{
			const int* a = cin[bits(cenv, 12, 4)];
			const int* b = cin[bits(cenv, 8, 4)];
			const int* c = cin[bits(cenv, 4, 4)];
			const int* d = cin[bits(cenv, 0, 4)];
			const u32 bias = bits(cenv, 16, 2), sub = bits(cenv, 18, 1), scale = bits(cenv, 20, 2);
			for (int k = 0; k < 3; k++) {
				int A = a[k] & 255, B = b[k] & 255, C = c[k] & 255, D = d[k];
				int r;
				if (bias == 3) {
					mStats.tevCompareStages++;
					u32 op = (scale << 1) | sub;
					int ca, cb;
					switch (op >> 1) {
					case 0: ca = a[0] & 255, cb = b[0] & 255; break;                                         // R8
					case 1: ca = ((a[1] & 255) << 8) | (a[0] & 255), cb = ((b[1] & 255) << 8) | (b[0] & 255); break; // GR16
					case 2:
						ca = ((a[2] & 255) << 16) | ((a[1] & 255) << 8) | (a[0] & 255);
						cb = ((b[2] & 255) << 16) | ((b[1] & 255) << 8) | (b[0] & 255);
						break; // BGR24
					default: ca = A, cb = B; break; // RGB8 per component
					}
					bool ok = (op & 1) ? ca == cb : ca > cb;
					r       = D + (ok ? C : 0);
				} else {
					int lerp = (A * (256 - C) + B * C) >> 8;
					r        = sub ? D - lerp : D + lerp;
					if (bias == 1) r += 128;
					if (bias == 2) r -= 128;
					switch (scale) {
					case 1: r *= 2; break;
					case 2: r *= 4; break;
					case 3: r /= 2; break;
					default: break;
					}
				}
				outC[k] = bits(cenv, 19, 1) ? clampi(r, 0, 255) : clampi(r, -1024, 1023);
			}
		}
		{
			int A = ain[bits(aenv, 13, 3)] & 255, B = ain[bits(aenv, 10, 3)] & 255, C = ain[bits(aenv, 7, 3)] & 255;
			int D = ain[bits(aenv, 4, 3)];
			const u32 bias = bits(aenv, 16, 2), sub = bits(aenv, 18, 1), scale = bits(aenv, 20, 2);
			int r;
			if (bias == 3) {
				mStats.tevCompareStages++;
				u32 op  = (scale << 1) | sub;
				bool ok = (op & 1) ? A == B : A > B;
				r       = D + (ok ? C : 0);
			} else {
				int lerp = (A * (256 - C) + B * C) >> 8;
				r        = sub ? D - lerp : D + lerp;
				if (bias == 1) r += 128;
				if (bias == 2) r -= 128;
				switch (scale) {
				case 1: r *= 2; break;
				case 2: r *= 4; break;
				case 3: r /= 2; break;
				default: break;
				}
			}
			outA = bits(aenv, 19, 1) ? clampi(r, 0, 255) : clampi(r, -1024, 1023);
		}
		for (int k = 0; k < 3; k++) regs[cdest][k] = outC[k];
		regs[adest][3] = outA;
	}
	int src[4];
	for (int k = 0; k < 4; k++) src[k] = clampi(regs[0][k], 0, 255);

	// alpha test
	const u32 ac = S.bp[0xF3];
	bool a0      = compare(bits(ac, 16, 3), src[3], (int)bits(ac, 0, 8));
	bool a1      = compare(bits(ac, 19, 3), src[3], (int)bits(ac, 8, 8));
	bool pass;
	switch (bits(ac, 22, 2)) {
	case 0: pass = a0 && a1; break;
	case 1: pass = a0 || a1; break;
	case 2: pass = a0 != a1; break;
	default: pass = a0 == a1; break;
	}
	if (!pass) {
		return;
	}
	if (!early && bits(zmode, 0, 1) && !compare(bits(zmode, 1, 3), zi, (int)mDepth[at])) {
		return;
	}
	if (bits(zmode, 0, 1) && bits(zmode, 4, 1)) {
		mDepth[at] = (u32)zi;
	}

	// blending
	const u32 cm0  = S.bp[0x41];
	const u32 fmt  = bits(pe, 0, 3);
	u32 dstPix     = mColor[at];
	int dst[4]     = { (int)(dstPix >> 24), (int)((dstPix >> 16) & 0xFF), (int)((dstPix >> 8) & 0xFF), (int)(dstPix & 0xFF) };
	if (fmt != 1) {
		dst[3] = 255; // no alpha in the frame buffer (RGB8, RGB565)
	}
	int res[4];
	if (bits(cm0, 11, 1)) { // subtract
		for (int k = 0; k < 4; k++) res[k] = clampi(dst[k] - src[k], 0, 255);
	} else if (bits(cm0, 0, 1)) {
		u32 sf = bits(cm0, 8, 3), df = bits(cm0, 5, 3);
		for (int k = 0; k < 4; k++) {
			int fs, fd;
			switch (sf) {
			case 0: fs = 0; break;
			case 1: fs = 255; break;
			case 2: fs = dst[k]; break;
			case 3: fs = 255 - dst[k]; break;
			case 4: fs = src[3]; break;
			case 5: fs = 255 - src[3]; break;
			case 6: fs = dst[3]; break;
			default: fs = 255 - dst[3]; break;
			}
			switch (df) {
			case 0: fd = 0; break;
			case 1: fd = 255; break;
			case 2: fd = src[k]; break;
			case 3: fd = 255 - src[k]; break;
			case 4: fd = src[3]; break;
			case 5: fd = 255 - src[3]; break;
			case 6: fd = dst[3]; break;
			default: fd = 255 - dst[3]; break;
			}
			res[k] = clampi((src[k] * fs + dst[k] * fd + 127) / 255, 0, 255);
		}
	} else if (bits(cm0, 1, 1)) {
		for (int k = 0; k < 4; k++) {
			int s = src[k], d = dst[k];
			switch (bits(cm0, 12, 4)) {
			case 0: res[k] = 0; break;
			case 1: res[k] = s & d; break;
			case 2: res[k] = s & ~d & 255; break;
			case 3: res[k] = s; break;
			case 4: res[k] = ~s & d & 255; break;
			case 5: res[k] = d; break;
			case 6: res[k] = s ^ d; break;
			case 7: res[k] = s | d; break;
			case 8: res[k] = ~(s | d) & 255; break;
			case 9: res[k] = ~(s ^ d) & 255; break;
			case 10: res[k] = ~d & 255; break;
			case 11: res[k] = (s | ~d) & 255; break;
			case 12: res[k] = ~s & 255; break;
			case 13: res[k] = (~s | d) & 255; break;
			case 14: res[k] = ~(s & d) & 255; break;
			default: res[k] = 255; break;
			}
		}
	} else {
		for (int k = 0; k < 4; k++) res[k] = src[k];
	}
	const u32 cm1 = S.bp[0x42];
	if (bits(cm1, 8, 1)) {
		res[3] = (int)bits(cm1, 0, 8); // constant destination alpha
	}
	if (fmt == 1) { // RGBA6
		for (int k = 0; k < 4; k++) res[k] = (res[k] & 0xFC) | (res[k] >> 6);
	} else if (fmt == 2) { // RGB565
		res[0] = (res[0] & 0xF8) | (res[0] >> 5);
		res[1] = (res[1] & 0xFC) | (res[1] >> 6);
		res[2] = (res[2] & 0xF8) | (res[2] >> 5);
	}
	u32 out = dstPix;
	if (bits(cm0, 3, 1)) {
		out = (out & 0x000000FF) | ((u32)res[0] << 24) | ((u32)res[1] << 16) | ((u32)res[2] << 8);
	}
	if (bits(cm0, 4, 1) && fmt == 1) {
		out = (out & 0xFFFFFF00) | (u32)res[3];
	}
	mColor[at] = out;
	mStats.pixelsWritten++;
}

// ---------------------------------------------------------------- copies out of the frame buffer

void Renderer::copy(const gpu::CopyInfo& info)
{
	const gpu::State& S = mCp.state();
	const u32 x0 = info.srcLeft, y0 = info.srcTop;
	const u32 w = info.width, h = info.height;
	if (info.toXfb) {
		mFrameW = w;
		mFrameH = h;
		mFrame.assign(w * h * 3, 0);
		for (u32 y = 0; y < h; y++) {
			for (u32 x = 0; x < w; x++) {
				u32 sx = x0 + x, sy = y0 + y;
				u32 c  = (sx < EFB_WIDTH && sy < EFB_HEIGHT) ? mColor[sy * EFB_WIDTH + sx] : 0;
				u8* o  = &mFrame[(y * w + x) * 3];
				o[0] = (u8)(c >> 24), o[1] = (u8)(c >> 16), o[2] = (u8)(c >> 8);
			}
		}
		mFrames++;
	} else {
		// texture copy: format, optional 2x2 box filter (half size), intensity conversion
		const u32 field    = bits(info.control, 3, 4);
		const u32 format   = field / 2 + (field & 1) * 8;
		const bool half    = bits(info.control, 9, 1);
		const bool intens  = bits(info.control, 15, 1);
		CopyTexture t;
		t.width  = half ? w / 2 : w;
		t.height = half ? h / 2 : h;
		t.format = format;
		t.rgba.assign(t.width * t.height, 0);
		for (u32 y = 0; y < t.height; y++) {
			for (u32 x = 0; x < t.width; x++) {
				int acc[4] = { 0, 0, 0, 0 };
				int n      = half ? 2 : 1;
				for (int j = 0; j < n; j++) {
					for (int i = 0; i < n; i++) {
						u32 sx = x0 + x * n + i, sy = y0 + y * n + j;
						u32 c  = (sx < EFB_WIDTH && sy < EFB_HEIGHT) ? mColor[sy * EFB_WIDTH + sx] : 0;
						acc[0] += c >> 24, acc[1] += (c >> 16) & 0xFF, acc[2] += (c >> 8) & 0xFF, acc[3] += c & 0xFF;
					}
				}
				for (int k = 0; k < 4; k++) acc[k] /= n * n;
				if (bits(S.bp[0x43], 0, 3) != 1) {
					acc[3] = 255;
				}
				u32 r = acc[0], g = acc[1], b = acc[2], a = acc[3];
				if (intens && (format <= 3 || format == 8)) {
					u32 yv = (u32)clampi((int)(0.257f * r + 0.504f * g + 0.098f * b + 16.0f), 0, 255);
					r = g = b = yv;
				}
				switch (format) {
				case 0:
				case 1:
				case 8: a = r; g = b = r; break; // R4/R8 (intensity: I4, I8)
				case 2:
				case 3: g = b = r; break;        // RA4/RA8 (IA4, IA8)
				case 7: r = g = b = a; break;    // A8
				case 9: r = b = g; break;        // G8
				case 10: r = g = b; break;       // B8
				default: break;
				}
				t.rgba[y * t.width + x] = pack(r, g, b, a);
			}
		}
		mCopies[info.dest & 0x3FFFFE0] = t;
	}
	if (info.clear) {
		const u32 ar = S.bp[0x4F], gb = S.bp[0x50];
		const u32 c  = pack(bits(ar, 0, 8), bits(gb, 8, 8), bits(gb, 0, 8), bits(ar, 8, 8));
		const u32 z  = S.bp[0x51] & 0xFFFFFF;
		const u32 cm0 = S.bp[0x41];
		const bool colorUpdate = bits(cm0, 3, 1), alphaUpdate = bits(cm0, 4, 1), zUpdate = bits(S.bp[0x40], 4, 1);
		for (u32 y = y0; y < y0 + h && y < EFB_HEIGHT; y++) {
			for (u32 x = x0; x < x0 + w && x < EFB_WIDTH; x++) {
				u32 at = y * EFB_WIDTH + x;
				u32 o  = mColor[at];
				if (colorUpdate) {
					o = (o & 0xFF) | (c & 0xFFFFFF00);
				}
				if (alphaUpdate) {
					o = (o & 0xFFFFFF00) | (c & 0xFF);
				}
				mColor[at] = o;
				if (zUpdate) {
					mDepth[at] = z;
				}
			}
		}
	}
}

// ================================================================== PNG output

static void put32be(std::vector<u8>& v, u32 x)
{
	v.push_back((u8)(x >> 24)), v.push_back((u8)(x >> 16)), v.push_back((u8)(x >> 8)), v.push_back((u8)x);
}

static void chunk(FILE* f, const char* type, const std::vector<u8>& data)
{
	std::vector<u8> c;
	put32be(c, (u32)data.size());
	c.insert(c.end(), type, type + 4);
	c.insert(c.end(), data.begin(), data.end());
	u32 crc = (u32)crc32(0, c.data() + 4, (uInt)(c.size() - 4));
	put32be(c, crc);
	fwrite(c.data(), 1, c.size(), f);
}

bool writePng(const char* path, const u8* rgb, u32 width, u32 height)
{
	FILE* f = fopen(path, "wb");
	if (!f) {
		return false;
	}
	static const u8 sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
	fwrite(sig, 1, 8, f);
	std::vector<u8> ihdr;
	put32be(ihdr, width);
	put32be(ihdr, height);
	ihdr.push_back(8), ihdr.push_back(2), ihdr.push_back(0), ihdr.push_back(0), ihdr.push_back(0);
	chunk(f, "IHDR", ihdr);
	std::vector<u8> raw;
	raw.reserve((width * 3 + 1) * height);
	for (u32 y = 0; y < height; y++) {
		raw.push_back(0);
		raw.insert(raw.end(), rgb + y * width * 3, rgb + (y + 1) * width * 3);
	}
	uLongf len = compressBound((uLong)raw.size());
	std::vector<u8> z(len);
	compress2(z.data(), &len, raw.data(), (uLong)raw.size(), 6);
	z.resize(len);
	chunk(f, "IDAT", z);
	chunk(f, "IEND", std::vector<u8>());
	fclose(f);
	return true;
}

} // namespace swr
