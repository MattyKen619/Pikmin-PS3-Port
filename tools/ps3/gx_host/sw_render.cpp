// PS3 port, host tools: software renderer for the GameCube's graphics processor (see sw_render.h).
//
// Register layouts follow the hardware as the GX library (src/gx) programs it.

#include "sw_render.h"

#include "../../../platform/ps3/gpu/gpu_tex.h"

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
			gpu::decodeTexture(src, k.width, k.height, k.format, tmem, k.tlut, k.tlutFormat, d.rgba.data(), k.width, gpu::TEXELS_RGBA);
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

	gpu::XfSetup xs;
	gpu::xfPrepare(S.xf, *call.layout, xs);
	if (xs.emboss) {
		mStats.embossTexgens++;
	}
	std::vector<OutVertex> v(call.count);
	for (u32 i = 0; i < call.count; i++) {
		gpu::xfTransform(xs, call.vertices[i], v[i]);
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
			float dp = plane == 0 ? p.pos[2] + p.pos[3] : p.pos[3] - 1e-5f;
			float dq = plane == 0 ? q.pos[2] + q.pos[3] : q.pos[3] - 1e-5f;
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
		float iw  = 1.0f / v[i]->pos[3];
		p[i].x    = xo + xs * v[i]->pos[0] * iw;
		p[i].y    = yo + ys * v[i]->pos[1] * iw;
		p[i].z    = zo + zs * v[i]->pos[2] * iw;
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
	u32 lastC = 0, lastA = 0;
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
		lastC = cdest, lastA = adest;
	}
	// (the last stage's results are the output, whichever register they went to)
	int src[4];
	for (int k = 0; k < 3; k++) src[k] = clampi(regs[lastC][k], 0, 255);
	src[3] = clampi(regs[lastA][3], 0, 255);

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
