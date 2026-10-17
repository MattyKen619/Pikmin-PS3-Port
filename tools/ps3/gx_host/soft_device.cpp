// PS3 port, host tools: software RSX for checking the renderer (see soft_device.h).
//
// Rasterization, depth, blending and texture sampling follow the reference renderer (sw_render.cpp)
// where the RSX would round differently, so that differences between the two point at the renderer's
// logic rather than at arithmetic details.

#include "soft_device.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

using gfx::u16;
using gfx::u32;
using gfx::u8;

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

SoftDevice::SoftDevice()
    : mScratch(16 * 1024 * 1024)
    , mFence(0)
    , mEfb(gfx::EFB_WIDTH * gfx::EFB_HEIGHT, 0xFF000000)
    , mDepth(gfx::EFB_WIDTH * gfx::EFB_HEIGHT, 0xFFFFFF)
    , mDisplay(640 * 480, 0xFF000000)
    , mFrames(0)
    , mUcode(NULL)
    , mVerts(NULL)
    , mStride(0)
    , mTexCoords(0)
{
	mEfbTexture.width  = gfx::EFB_WIDTH;
	mEfbTexture.height = gfx::EFB_HEIGHT;
	mEfbTexture.pitch  = gfx::EFB_WIDTH * 4;
	mEfbTexture.texels = &mEfb[0];
	mEfbTexture.bytes  = 0;
	mEfbTexture.impl   = NULL;
	memset(&mState, 0, sizeof(mState));
	memset(mTex, 0, sizeof(mTex));
	memset(mSampler, 0, sizeof(mSampler));
	memset(&mStats, 0, sizeof(mStats));
	setTarget(NULL);
}

SoftDevice::~SoftDevice() { }

gfx::Texture* SoftDevice::createTexture(u32 width, u32 height)
{
	gfx::Texture* t = new gfx::Texture;
	t->width        = width;
	t->height       = height;
	t->pitch        = (width * 4 + 63) & ~63u;
	t->bytes        = t->pitch * height;
	t->texels       = new u32[t->pitch / 4 * height];
	t->impl         = NULL;
	memset(t->texels, 0, t->bytes);
	return t;
}

void SoftDevice::destroyTexture(gfx::Texture* t)
{
	for (int i = 0; i < 16; i++) {
		if (mTex[i] == t) {
			mTex[i] = NULL;
		}
	}
	delete[] t->texels;
	delete t;
}

void SoftDevice::setTarget(const gfx::Texture* t)
{
	if (!t) {
		mColor    = &mEfb[0];
		mPitch    = gfx::EFB_WIDTH;
		mWidth    = gfx::EFB_WIDTH;
		mHeight   = gfx::EFB_HEIGHT;
		mHasDepth = true;
	} else {
		mColor    = t->texels;
		mPitch    = t->pitch / 4;
		mWidth    = t->width;
		mHeight   = t->height;
		mHasDepth = false;
	}
}

bool SoftDevice::beginDisplay(u32* width, u32* height)
{
	mColor    = &mDisplay[0];
	mPitch    = 640;
	mWidth    = 640;
	mHeight   = 480;
	mHasDepth = false;
	*width    = 640;
	*height   = 480;
	return true;
}

void SoftDevice::setProgram(const void* ucode, const gfx::FragmentProgram& fp)
{
	mUcode = (const u32*)ucode;
	mFp    = fp;
}

void SoftDevice::setTexture(u32 unit, const gfx::Texture* t, const gfx::Sampler& s)
{
	mTex[unit & 15]     = t;
	mSampler[unit & 15] = s;
}

void SoftDevice::setVertices(const void* base, u32 stride, u32 texCoords)
{
	mVerts     = (const u8*)base;
	mStride    = stride;
	mTexCoords = texCoords;
}

void SoftDevice::clear(const u16 rect[4], u32 argb, u32 depth, bool color, bool alpha, bool depthToo)
{
	for (u32 y = rect[1]; y < (u32)rect[1] + rect[3] && y < mHeight; y++) {
		for (u32 x = rect[0]; x < (u32)rect[0] + rect[2] && x < mWidth; x++) {
			u32& c = mColor[y * mPitch + x];
			if (color) {
				c = (c & 0xFF000000) | (argb & 0xFFFFFF);
			}
			if (alpha) {
				c = (c & 0xFFFFFF) | (argb & 0xFF000000);
			}
			if (depthToo && mHasDepth) {
				mDepth[y * gfx::EFB_WIDTH + x] = depth & 0xFFFFFF;
			}
		}
	}
}

bool SoftDevice::readTarget(u32* out, u32 x, u32 y, u32 width, u32 height)
{
	for (u32 j = 0; j < height; j++) {
		for (u32 i = 0; i < width; i++) {
			out[j * width + i] = (x + i < mWidth && y + j < mHeight) ? mColor[(y + j) * mPitch + x + i] : 0;
		}
	}
	return true;
}

// ================================================================== vertices and triangles

void SoftDevice::fetch(unsigned short index, Vert& v)
{
	const u8* p    = mVerts + index * mStride;
	const float* f = (const float*)p;
	for (int k = 0; k < 4; k++) {
		v.pos[k] = f[k];
	}
	memset(v.in, 0, sizeof(v.in));
	for (int k = 0; k < 4; k++) {
		v.in[1][k] = p[16 + k] / 255.0f;
		v.in[2][k] = p[20 + k] / 255.0f;
	}
	const float* tc = (const float*)(p + 24);
	for (u32 t = 0; t < mTexCoords; t++) {
		v.in[4 + t][0] = tc[t * 3];
		v.in[4 + t][1] = tc[t * 3 + 1];
		v.in[4 + t][2] = tc[t * 3 + 2];
		v.in[4 + t][3] = tc[t * 3 + 2]; // (the vertex program passes (s, t, q, q))
	}
}

void SoftDevice::drawTriangles(const unsigned short* indices, u32 count)
{
	for (u32 i = 0; i + 2 < count; i += 3) {
		Vert a, b, c;
		fetch(indices[i], a);
		fetch(indices[i + 1], b);
		fetch(indices[i + 2], c);
		triangle(a, b, c);
	}
}

static void lerpVert(const SoftDevice* /*unused*/, const float* a, const float* b, float t, float* out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		out[i] = a[i] + (b[i] - a[i]) * t;
	}
}

void SoftDevice::triangle(const Vert& a, const Vert& b, const Vert& c)
{
	mStats.triangles++;
	// clip against the near plane (z >= -w) and w > 0, as the reference renderer does
	Vert in[8], out[8];
	int nin = 3;
	in[0] = a, in[1] = b, in[2] = c;
	const size_t nf = sizeof(Vert) / sizeof(float);
	for (int plane = 0; plane < 2; plane++) {
		int nout = 0;
		for (int i = 0; i < nin; i++) {
			const Vert& p = in[i];
			const Vert& q = in[(i + 1) % nin];
			float dp      = plane == 0 ? p.pos[2] + p.pos[3] : p.pos[3] - 1e-5f;
			float dq      = plane == 0 ? q.pos[2] + q.pos[3] : q.pos[3] - 1e-5f;
			if (dp >= 0) {
				out[nout++] = p;
			}
			if ((dp >= 0) != (dq >= 0)) {
				lerpVert(this, (const float*)&p, (const float*)&q, dp / (dp - dq), (float*)&out[nout++], nf);
			}
		}
		nin = nout;
		for (int i = 0; i < nin; i++) {
			in[i] = out[i];
		}
		if (nin < 3) {
			return;
		}
	}
	for (int i = 1; i + 1 < nin; i++) {
		const Vert* t[3] = { &in[0], &in[i], &in[i + 1] };
		rasterize(t);
	}
}

void SoftDevice::rasterize(const Vert* v[3])
{
	const gfx::RenderState& S = mState;
	float px[3], py[3], pz[3], iw[3];
	for (int i = 0; i < 3; i++) {
		iw[i] = 1.0f / v[i]->pos[3];
		px[i] = S.viewportOffset[0] + S.viewportScale[0] * v[i]->pos[0] * iw[i];
		py[i] = S.viewportOffset[1] + S.viewportScale[1] * v[i]->pos[1] * iw[i];
		pz[i] = S.viewportOffset[2] + S.viewportScale[2] * v[i]->pos[2] * iw[i];
	}
	float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
	if (area == 0) {
		return;
	}
	int sx0 = S.scissor[0], sy0 = S.scissor[1];
	int sx1 = S.scissor[0] + S.scissor[2] - 1, sy1 = S.scissor[1] + S.scissor[3] - 1;
	sx1 = sx1 < (int)mWidth - 1 ? sx1 : (int)mWidth - 1;
	sy1 = sy1 < (int)mHeight - 1 ? sy1 : (int)mHeight - 1;
	int minX = (int)floorf(fminf(px[0], fminf(px[1], px[2])));
	int maxX = (int)ceilf(fmaxf(px[0], fmaxf(px[1], px[2])));
	int minY = (int)floorf(fminf(py[0], fminf(py[1], py[2])));
	int maxY = (int)ceilf(fmaxf(py[0], fmaxf(py[1], py[2])));
	minX = minX < sx0 ? sx0 : minX;
	minY = minY < sy0 ? sy0 : minY;
	maxX = maxX > sx1 ? sx1 : maxX;
	maxY = maxY > sy1 ? sy1 : maxY;
	if (minX > maxX || minY > maxY) {
		return;
	}
	const float inv = 1.0f / area;
	float in[16][4];
	for (int y = minY; y <= maxY; y++) {
		for (int x = minX; x <= maxX; x++) {
			float cx = x + 0.5f, cy = y + 0.5f;
			float w0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) * inv;
			float w1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) * inv;
			float w2 = 1.0f - w0 - w1;
			if (w0 < 0 || w1 < 0 || w2 < 0) {
				continue;
			}
			float z   = w0 * pz[0] + w1 * pz[1] + w2 * pz[2];
			float pw0 = w0 * iw[0], pw1 = w1 * iw[1], pw2 = w2 * iw[2];
			float sum = pw0 + pw1 + pw2;
			pw0 /= sum, pw1 /= sum, pw2 /= sum;
			for (int r = 1; r < 12; r++) {
				for (int k = 0; k < 4; k++) {
					in[r][k] = pw0 * v[0]->in[r][k] + pw1 * v[1]->in[r][k] + pw2 * v[2]->in[r][k];
				}
			}
			in[0][0] = cx, in[0][1] = cy, in[0][2] = z, in[0][3] = 1;
			pixel(x, y, z, in);
		}
	}
}

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

static int factor(u8 f, const int* src, const int* dst, int k)
{
	switch (f) {
	case gfx::BLEND_ZERO: return 0;
	case gfx::BLEND_ONE: return 255;
	case gfx::BLEND_SRC_COLOR: return src[k];
	case gfx::BLEND_INV_SRC_COLOR: return 255 - src[k];
	case gfx::BLEND_DST_COLOR: return dst[k];
	case gfx::BLEND_INV_DST_COLOR: return 255 - dst[k];
	case gfx::BLEND_SRC_ALPHA: return src[3];
	case gfx::BLEND_INV_SRC_ALPHA: return 255 - src[3];
	case gfx::BLEND_DST_ALPHA: return dst[3];
	default: return 255 - dst[3];
	}
}

void SoftDevice::pixel(int x, int y, float zf, const float in[16][4])
{
	mStats.pixels++;
	float out[4];
	if (!runProgram(in, out)) {
		mStats.killed++;
		return;
	}
	const gfx::RenderState& S = mState;
	if (mHasDepth && S.depthTest) {
		float zc = zf < 0 ? 0 : zf > 1 ? 1 : zf;
		int zi   = clampi((int)(zc * 16777215.0f + 0.5f), 0, 0xFFFFFF);
		u32& d   = mDepth[y * gfx::EFB_WIDTH + x];
		if (!compare(S.depthFunc, zi, (int)d)) {
			return;
		}
		if (S.depthWrite) {
			d = (u32)zi;
		}
	}
	int src[4];
	for (int k = 0; k < 4; k++) {
		src[k] = clampi((int)(out[k] * 255.0f + 0.5f), 0, 255);
	}
	u32& pix   = mColor[y * mPitch + x];
	int dst[4] = { (int)((pix >> 16) & 0xFF), (int)((pix >> 8) & 0xFF), (int)(pix & 0xFF), (int)(pix >> 24) };
	int res[4];
	if (S.blend == 2) {
		for (int k = 0; k < 4; k++) res[k] = clampi(dst[k] - src[k], 0, 255);
	} else if (S.blend == 1) {
		for (int k = 0; k < 4; k++) {
			int fs = factor(S.srcFactor, src, dst, k), fd = factor(S.dstFactor, src, dst, k);
			res[k] = clampi((src[k] * fs + dst[k] * fd + 127) / 255, 0, 255);
		}
	} else if (S.logicEnable) {
		for (int k = 0; k < 4; k++) {
			int s = src[k], d = dst[k];
			switch (S.logicOp) {
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
	u32 o = pix;
	if (S.colorWrite) {
		o = (o & 0xFF000000) | ((u32)res[0] << 16) | ((u32)res[1] << 8) | (u32)res[2];
	}
	if (S.alphaWrite) {
		o = (o & 0xFFFFFF) | ((u32)res[3] << 24);
	}
	pix = o;
	mStats.written++;
}

// ================================================================== textures

static inline u32 wrapCoord(int v, u32 size, u32 mode)
{
	switch (mode) {
	case gfx::WRAP_CLAMP: return (u32)clampi(v, 0, (int)size - 1);
	case gfx::WRAP_MIRROR:
	{
		int period = (int)size * 2;
		int m      = ((v % period) + period) % period;
		return (u32)(m < (int)size ? m : period - 1 - m);
	}
	default: return (u32)(((v % (int)size) + (int)size) % (int)size);
	}
}

void SoftDevice::sample(u32 unit, float s, float t, float out[4])
{
	const gfx::Texture* tex = mTex[unit & 15];
	if (!tex) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return;
	}
	const gfx::Sampler& sm = mSampler[unit & 15];
	const u32 pitch        = tex->pitch / 4;
	float u = s * tex->width, v = t * tex->height;
	// (the reference renderer uses the magnification filter for everything: the textures have one level)
	if (!sm.linearMag) {
		u32 x = wrapCoord((int)floorf(u), tex->width, sm.wrapS);
		u32 y = wrapCoord((int)floorf(v), tex->height, sm.wrapT);
		u32 c = tex->texels[y * pitch + x];
		out[0] = ((c >> 16) & 0xFF) / 255.0f, out[1] = ((c >> 8) & 0xFF) / 255.0f, out[2] = (c & 0xFF) / 255.0f, out[3] = (c >> 24) / 255.0f;
		return;
	}
	u -= 0.5f, v -= 0.5f;
	int x0 = (int)floorf(u), y0 = (int)floorf(v);
	float fx = u - x0, fy = v - y0;
	u32 xs[2] = { wrapCoord(x0, tex->width, sm.wrapS), wrapCoord(x0 + 1, tex->width, sm.wrapS) };
	u32 ys[2] = { wrapCoord(y0, tex->height, sm.wrapT), wrapCoord(y0 + 1, tex->height, sm.wrapT) };
	float acc[4] = { 0, 0, 0, 0 };
	for (int j = 0; j < 2; j++) {
		for (int i = 0; i < 2; i++) {
			float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
			u32 c   = tex->texels[ys[j] * pitch + xs[i]];
			acc[0] += w * ((c >> 16) & 0xFF), acc[1] += w * ((c >> 8) & 0xFF), acc[2] += w * (c & 0xFF), acc[3] += w * (c >> 24);
		}
	}
	for (int k = 0; k < 4; k++) {
		out[k] = acc[k] / 255.0f;
	}
}

// ================================================================== fragment programs (NV40 microcode)

static float wordFloat(u32 stored)
{
	union {
		u32 u;
		float f;
	} x;
	x.u = gfx::fpWord(stored);
	return x.f;
}

static bool condition(u32 cond, float v)
{
	switch (cond) {
	case 0: return false;
	case 1: return v < 0;
	case 2: return v == 0;
	case 3: return v <= 0;
	case 4: return v > 0;
	case 5: return v != 0;
	case 6: return v >= 0;
	default: return true;
	}
}

bool SoftDevice::runProgram(const float in[16][4], float out[4])
{
	if (!mUcode) {
		out[0] = out[1] = out[2] = out[3] = 0;
		return true;
	}
	float R[64][4];
	memset(R, 0, sizeof(R));
	float cc[4] = { 0, 0, 0, 0 };
	const u32 slots = (u32)mFp.ucode.size() / 4;
	for (u32 pc = 0; pc < slots;) {
		const u32* ins = mUcode + pc * 4;
		const u32 w0 = gfx::fpWord(ins[0]), w1 = gfx::fpWord(ins[1]), w2 = gfx::fpWord(ins[2]), w3 = gfx::fpWord(ins[3]);
		const u32 op = (w0 >> 24) & 0x3F;
		const u32 sw[3] = { w1, w2, w3 };
		const u32 absBit[3] = { (w1 >> 29) & 1, (w2 >> 18) & 1, (w3 >> 18) & 1 };
		bool hasConst = false;
		float s[3][4];
		for (int i = 0; i < 3; i++) {
			const u32 w = sw[i];
			float raw[4];
			switch (w & 3) {
			case 0: memcpy(raw, R[(w >> 2) & 63], sizeof(raw)); break;
			case 1: memcpy(raw, in[(w0 >> 13) & 15], sizeof(raw)); break;
			case 2:
				hasConst = true;
				for (int k = 0; k < 4; k++) raw[k] = wordFloat(mUcode[(pc + 1) * 4 + k]);
				break;
			default: memset(raw, 0, sizeof(raw)); break;
			}
			for (int k = 0; k < 4; k++) {
				float v = raw[(w >> (9 + k * 2)) & 3];
				if (absBit[i]) v = fabsf(v);
				if ((w >> 17) & 1) v = -v;
				s[i][k] = v;
			}
		}
		float r[4] = { 0, 0, 0, 0 };
		bool write = true;
		switch (op) {
		case 0x00: write = false; break;                                                     // NOP
		case 0x01: memcpy(r, s[0], sizeof(r)); break;                                         // MOV
		case 0x02: for (int k = 0; k < 4; k++) r[k] = s[0][k] * s[1][k]; break;               // MUL
		case 0x03: for (int k = 0; k < 4; k++) r[k] = s[0][k] + s[1][k]; break;               // ADD
		case 0x04: for (int k = 0; k < 4; k++) r[k] = s[0][k] * s[1][k] + s[2][k]; break;     // MAD
		case 0x05: r[0] = r[1] = r[2] = r[3] = s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2]; break; // DP3
		case 0x06: r[0] = r[1] = r[2] = r[3] = s[0][0] * s[1][0] + s[0][1] * s[1][1] + s[0][2] * s[1][2] + s[0][3] * s[1][3]; break;
		case 0x08: for (int k = 0; k < 4; k++) r[k] = fminf(s[0][k], s[1][k]); break;
		case 0x09: for (int k = 0; k < 4; k++) r[k] = fmaxf(s[0][k], s[1][k]); break;
		case 0x0A: for (int k = 0; k < 4; k++) r[k] = s[0][k] < s[1][k] ? 1.0f : 0.0f; break;  // SLT
		case 0x0B: for (int k = 0; k < 4; k++) r[k] = s[0][k] >= s[1][k] ? 1.0f : 0.0f; break; // SGE
		case 0x0C: for (int k = 0; k < 4; k++) r[k] = s[0][k] <= s[1][k] ? 1.0f : 0.0f; break; // SLE
		case 0x0D: for (int k = 0; k < 4; k++) r[k] = s[0][k] > s[1][k] ? 1.0f : 0.0f; break;  // SGT
		case 0x0E: for (int k = 0; k < 4; k++) r[k] = s[0][k] != s[1][k] ? 1.0f : 0.0f; break; // SNE
		case 0x0F: for (int k = 0; k < 4; k++) r[k] = s[0][k] == s[1][k] ? 1.0f : 0.0f; break; // SEQ
		case 0x10: for (int k = 0; k < 4; k++) r[k] = s[0][k] - floorf(s[0][k]); break;       // FRC
		case 0x11: for (int k = 0; k < 4; k++) r[k] = floorf(s[0][k]); break;                 // FLR
		case 0x12:                                                                           // KIL
		{
			const u32 cond = (w1 >> 18) & 7;
			for (int k = 0; k < 4; k++) {
				if (condition(cond, cc[(w1 >> (21 + k * 2)) & 3])) {
					return false;
				}
			}
			write = false;
			break;
		}
		case 0x17:                                                                           // TEX
		case 0x18:                                                                           // TXP
		{
			float x = s[0][0], y = s[0][1];
			if (op == 0x18 && s[0][3] != 0) {
				x /= s[0][3], y /= s[0][3];
			}
			sample((w0 >> 17) & 15, x, y, r);
			break;
		}
		case 0x1A: r[0] = r[1] = r[2] = r[3] = 1.0f / s[0][0]; break; // RCP
		case 0x38: r[0] = r[1] = r[2] = r[3] = s[0][0] * s[1][0] + s[0][1] * s[1][1]; break; // DP2
		default:
			mStats.programErrors++;
			write = false;
			break;
		}
		if (write) {
			static const float kScale[8] = { 1, 2, 4, 8, 1, 0.5f, 0.25f, 0.125f };
			const float scale  = kScale[(w2 >> 28) & 7];
			const bool sat     = (w0 >> 31) & 1;
			const bool none    = (w0 >> 30) & 1;
			const u32 reg      = (w0 >> 1) & 63;
			const u32 mask     = (w0 >> 9) & 15;
			const bool ccWrite = (w0 >> 8) & 1;
			const u32 cond     = (w1 >> 18) & 7;
			for (int k = 0; k < 4; k++) {
				if (!(mask & (1u << k)) || !condition(cond, cc[(w1 >> (21 + k * 2)) & 3])) {
					continue;
				}
				float v = r[k] * scale;
				if (sat) {
					v = v < 0 ? 0 : v > 1 ? 1 : v;
				}
				if (ccWrite) {
					cc[k] = v;
				}
				if (!none) {
					R[reg][k] = v;
				}
			}
		}
		pc += hasConst ? 2 : 1;
		if (w0 & 1) {
			break; // (end of the program)
		}
	}
	memcpy(out, R[0], sizeof(float) * 4);
	return true;
}
