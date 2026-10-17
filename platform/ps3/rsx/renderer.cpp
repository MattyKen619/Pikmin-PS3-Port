// PS3 port: the GameCube's graphics commands drawn through a Device (see renderer.h).
//
// Register layouts follow the hardware as the GX library (src/gx) programs it; the reference for every
// rule here is the host tools' software renderer (tools/ps3/gx_host/sw_render.cpp).

#include "renderer.h"

#include "../gpu/gpu_tex.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gfx {

namespace {

inline u32 bits(u32 v, u32 shift, u32 count) { return (v >> shift) & ((1u << count) - 1); }
inline float asFloat(u32 w)
{
	union {
		u32 u;
		float f;
	} x;
	x.u = w;
	return x.f;
}
inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
inline u64 alignUp(u64 v, u32 a) { return (v + a - 1) & ~(u64)(a - 1); }
inline u8 colorByte(float c)
{
	int v = (int)(c * 255.0f + 0.5f);
	return (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}

enum {
	BATCH_RESERVE   = 64 * 1024,
	TEXTURE_BUDGET  = 96 * 1024 * 1024, // device memory for decoded textures before old ones are evicted
	MAX_BATCH_VERTS = 65535,
};

const u8 kBlendSrc[8] = { BLEND_ZERO, BLEND_ONE, BLEND_DST_COLOR, BLEND_INV_DST_COLOR, BLEND_SRC_ALPHA, BLEND_INV_SRC_ALPHA, BLEND_DST_ALPHA, BLEND_INV_DST_ALPHA };
const u8 kBlendDst[8] = { BLEND_ZERO, BLEND_ONE, BLEND_SRC_COLOR, BLEND_INV_SRC_COLOR, BLEND_SRC_ALPHA, BLEND_INV_SRC_ALPHA, BLEND_DST_ALPHA, BLEND_INV_DST_ALPHA };

// TEV register writes (BP 0xE0-0xE7): RA (red bits 0-10, alpha 12-22) and BG (blue 0-10, green 12-22) of
// register n = (reg - 0xE0) / 2; bit 23 selects the constant (KONST) registers.
void tevRegFromBp(int regs[4][4], int konst[4][4], u32 reg, u32 value)
{
	const int n   = (int)(reg - 0xE0) >> 1;
	int(*dst)[4] = ((value >> 23) & 1) ? konst : regs;
	int lo       = (int)(value & 0x7FF);
	int hi       = (int)((value >> 12) & 0x7FF);
	if (lo & 0x400) {
		lo -= 0x800;
	}
	if (hi & 0x400) {
		hi -= 0x800;
	}
	if ((reg & 1) == 0) {
		dst[n][0] = lo, dst[n][3] = hi;
	} else {
		dst[n][2] = lo, dst[n][1] = hi;
	}
}

} // namespace

bool Renderer::TexKey::operator<(const TexKey& o) const
{
	if (address != o.address) return address < o.address;
	if (width != o.width) return width < o.width;
	if (height != o.height) return height < o.height;
	if (format != o.format) return format < o.format;
	if (tlutFormat != o.tlutFormat) return tlutFormat < o.tlutFormat;
	return tlutHash < o.tlutHash;
}

Renderer::Renderer(gpu::CommandProcessor& cp, gpu::Memory& memory, Device& device)
    : mCp(cp)
    , mMem(memory)
    , mDev(device)
    , mLog(NULL)
    , mFrame(0)
    , mHead(0)
    , mTail(0)
    , mLastMark(0)
    , mFenceCounter(0)
    , mDirty(true)
    , mDrawable(false)
    , mStateValid(false)
    , mCull(0)
    , mProgram(NULL)
    , mProgramBound(false)
    , mMissing(NULL)
    , mTextureBytes(0)
    , mXfSerial(0)
    , mXfValid(false)
{
	memset(&mStats, 0, sizeof(mStats));
	memset(mTevReg, 0, sizeof(mTevReg));
	memset(mKonst, 0, sizeof(mKonst));
	memset(mViewport, 0, sizeof(mViewport));
	memset(mParams, 0, sizeof(mParams));
	memset(&mState, 0, sizeof(mState));
	mTmem = (u8*)calloc(1, 0x100000);
	mScratch     = mDev.scratchBase();
	mScratchSize = mDev.scratchSize();
	mBatch.active = false;
	mBatch.base   = NULL;
	mBatch.reserved = mBatch.stride = mBatch.texCoords = mBatch.vertexCount = 0;
	for (int i = 0; i < 8; i++) {
		mBound[i].valid = false;
		mBound[i].tex   = NULL;
	}
	memset(mCopyProgramReady, 0, sizeof(mCopyProgramReady));
}

Renderer::~Renderer()
{
	free(mTmem);
	for (std::map<u32, std::vector<ProgramEntry*> >::iterator it = mPrograms.begin(); it != mPrograms.end(); ++it) {
		for (size_t i = 0; i < it->second.size(); i++) {
			delete it->second[i];
		}
	}
}

void Renderer::log(const char* fmt, ...)
{
	if (!mLog) {
		return;
	}
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	mLog(buf);
}

void Renderer::begin()
{
	const gpu::State& S = mCp.state();
	for (u32 r = 0xE0; r < 0xE8; r++) {
		tevRegFromBp(mTevReg, mKonst, r, S.bp[r]);
	}
	mDirty   = true;
	mXfValid = false;
}

void Renderer::flush()
{
	flushBatch();
	mark();
	mDev.kick();
}

bool Renderer::readEfb(u32* out, u32 x, u32 y, u32 width, u32 height)
{
	flush();
	return mDev.readTarget(out, x, y, width, height);
}

// ================================================================== scratch memory
//
// Positions in the ring count up forever (mHead: the next free byte, mTail: the oldest byte the chip may
// still read); the byte at position p is at mScratch[p % size]. An allocation never wraps: it skips to
// the start instead. A marker (a fence after the commands that use the memory) releases everything
// allocated before it once the chip reaches the fence.

void* Renderer::ringAlloc(u32 bytes, u32 align)
{
	for (;;) {
		u64 pos  = alignUp(mHead, align);
		u32 phys = (u32)(pos % mScratchSize);
		if (phys + bytes > mScratchSize) {
			pos += mScratchSize - phys;
			phys = 0;
		}
		if (pos + bytes - mTail <= mScratchSize) {
			mHead = pos + bytes;
			return mScratch + phys;
		}
		// Not enough room: release what the chip has finished with, and wait if that is not enough. (No
		// batch is pending here: everything allocated is used by submitted commands, so marking is safe.)
		release();
		if (pos + bytes - mTail <= mScratchSize) {
			continue;
		}
		if (mLastMark < mHead) {
			mark();
		}
		mDev.kick();
		mStats.fenceWaits++;
		while (!mMarkers.empty() && (int)(mDev.fenceReached() - mMarkers.front().fence) < 0) {
			mDev.pause();
		}
		release();
	}
}

void Renderer::mark()
{
	if (mLastMark == mHead && !mMarkers.empty()) {
		return;
	}
	Marker m;
	m.fence = ++mFenceCounter;
	m.pos   = mHead;
	mDev.writeFence(m.fence);
	mMarkers.push_back(m);
	mLastMark = mHead;
}

void Renderer::release()
{
	const u32 reached = mDev.fenceReached();
	size_t n          = 0;
	while (n < mMarkers.size() && (int)(reached - mMarkers[n].fence) >= 0) {
		mTail = mMarkers[n].pos;
		n++;
	}
	if (n) {
		mMarkers.erase(mMarkers.begin(), mMarkers.begin() + n);
	}
	for (size_t i = 0; i < mRetired.size();) {
		if ((int)(reached - mRetired[i].fence) >= 0) {
			mTextureBytes -= mRetired[i].tex->bytes;
			mDev.destroyTexture(mRetired[i].tex);
			mRetired[i] = mRetired.back();
			mRetired.pop_back();
		} else {
			i++;
		}
	}
}

// A texture the chip may still read: destroyed after the next marker's fence.
void Renderer::retire(Texture* t)
{
	if (!t) {
		return;
	}
	for (int i = 0; i < 8; i++) {
		if (mBound[i].tex == t) {
			mBound[i].valid = false;
			mBound[i].tex   = NULL;
		}
	}
	Retired r;
	r.tex   = t;
	r.fence = mFenceCounter + 1;
	mRetired.push_back(r);
}

// ================================================================== batches

u8* Renderer::batchVertices(u32 count, u32 texCoords, u32* first)
{
	const u32 stride = VERTEX_BASE_BYTES + VERTEX_TEX_BYTES * texCoords;
	if (mBatch.active
	    && (texCoords != mBatch.texCoords || mBatch.vertexCount + count > MAX_BATCH_VERTS
	        || (mBatch.vertexCount + count) * stride > mBatch.reserved)) {
		flushBatch();
	}
	if (!mBatch.active) {
		u32 need = count * stride;
		if (need > mScratchSize / 4) {
			return NULL;
		}
		u32 reserve     = need > (u32)BATCH_RESERVE ? need : (u32)BATCH_RESERVE;
		mBatch.base     = (u8*)ringAlloc(reserve, 16);
		mBatch.reserved = reserve;
		mBatch.stride   = stride;
		mBatch.texCoords = texCoords;
		mBatch.vertexCount = 0;
		mBatch.indices.clear();
		mBatch.active = true;
	}
	*first = mBatch.vertexCount;
	return mBatch.base + mBatch.vertexCount * stride;
}

void Renderer::flushBatch()
{
	if (!mBatch.active) {
		return;
	}
	// The unused part of the reservation goes back to the ring (nothing was allocated after it).
	mHead -= mBatch.reserved - mBatch.vertexCount * mBatch.stride;
	if (!mBatch.indices.empty()) {
		mDev.setVertices(mBatch.base, mBatch.stride, mBatch.texCoords);
		mDev.drawTriangles(&mBatch.indices[0], (u32)mBatch.indices.size());
		mStats.batches++;
	}
	mBatch.active = false;
	mBatch.indices.clear();
	if (mHead - mLastMark > mScratchSize / 8) {
		mark();
		mDev.kick();
	}
}

// ================================================================== state

void Renderer::invalidateDeviceState()
{
	mStateValid   = false;
	mProgramBound = false;
	for (int i = 0; i < 8; i++) {
		mBound[i].valid = false;
	}
	mDirty = true;
}

void Renderer::bindProgram(const FragmentProgram& fp, const float (*params)[4])
{
	const u32 bytes = (u32)fp.ucode.size() * 4;
	u32* dst        = (u32*)ringAlloc(bytes, 128);
	memcpy(dst, &fp.ucode[0], bytes);
	if (params) {
		for (size_t i = 0; i < fp.relocs.size(); i++) {
			const FragmentProgram::Reloc& r = fp.relocs[i];
			for (int k = 0; k < 4; k++) {
				dst[r.slot * 4 + k] = fpConstantWord(params[r.index][k]);
			}
		}
	}
	mDev.setProgram(dst, fp);
	mStats.programBinds++;
}

const Texture* Renderer::missingTexture()
{
	if (!mMissing) {
		mMissing = mDev.createTexture(1, 1);
		if (mMissing) {
			mMissing->texels[0] = 0xFFFF00FF; // magenta: a texture that could not be read
			mDev.texturesChanged();
		}
	}
	return mMissing;
}

const Texture* Renderer::lookupTexture(u32 map, Sampler& sampler)
{
	const u32* bp   = mCp.state().bp;
	const u32 base  = map < 4 ? 0x80 + map : 0xA0 + (map - 4);
	const u32 mode0 = bp[base];
	const u32 img0  = bp[base + 0x08];
	const u32 img3  = bp[base + 0x14];
	const u32 tlutR = bp[base + 0x18];
	sampler.wrapS     = (u8)bits(mode0, 0, 2);
	sampler.wrapT     = (u8)bits(mode0, 2, 2);
	sampler.linearMag = (u8)bits(mode0, 4, 1);
	sampler.linearMin = (u8)(bits(mode0, 5, 3) >= 4);
	if (sampler.wrapS > 2) sampler.wrapS = 0;
	if (sampler.wrapT > 2) sampler.wrapT = 0;

	TexKey k;
	k.address = bits(img3, 0, 21) << 5;
	k.width   = bits(img0, 0, 10) + 1;
	k.height  = bits(img0, 10, 10) + 1;
	k.format  = bits(img0, 20, 4);

	// a copy out of the frame buffer to this address?
	std::map<u32, CopyEntry>::iterator ci = mCopies.find(k.address);
	if (ci != mCopies.end()) {
		CopyEntry& c = ci->second;
		bool valid   = true;
		if (c.memKnown && c.checkedFrame != mFrame) {
			// (the game may have loaded something else there since)
			u32 size      = gpu::CommandProcessor::textureSize(c.format, c.width, c.height, 1);
			const u8* src = mMem.read(k.address, size, gpu::MEM_TEXTURE);
			if (src && gpu::hashMemory(src, size) != c.memHash) {
				valid = false;
			}
			c.checkedFrame = mFrame;
		}
		if (valid) {
			mStats.copyUses++;
			return c.tex;
		}
		retire(c.tex);
		mCopies.erase(ci);
	}

	const bool ci8 = gpu::isIndexedFormat(k.format);
	k.tlutFormat   = ci8 ? bits(tlutR, 10, 2) : 0;
	k.tlutHash     = 0;
	u32 tlut       = 0;
	if (ci8) {
		tlut = bits(tlutR, 0, 10) << 9;
		u32 bytes = gpu::paletteEntries(k.format) * 2;
		if (tlut + bytes > 0x100000) {
			bytes = 0x100000 - tlut;
		}
		k.tlutHash = gpu::hashMemory(mTmem + tlut, bytes);
	}
	TexEntry& e = mTextures[k];
	if (e.tex && e.checkedFrame == mFrame) {
		e.usedFrame = mFrame;
		return e.tex;
	}
	const u32 size = gpu::CommandProcessor::textureSize(k.format, k.width, k.height, 1);
	const u8* src  = k.address ? mMem.read(k.address, size, gpu::MEM_TEXTURE) : NULL;
	if (!src) {
		mStats.missingTextures++;
		if (!e.tex) {
			mTextures.erase(k);
		}
		return missingTexture();
	}
	const u64 hash = gpu::hashMemory(src, size);
	if (e.tex && hash == e.hash) {
		e.checkedFrame = mFrame;
		e.usedFrame    = mFrame;
		return e.tex;
	}
	Texture* t = mDev.createTexture(k.width, k.height);
	if (!t) {
		evictTextures();
		release();
		t = mDev.createTexture(k.width, k.height);
		if (!t) {
			log("[rsx] out of texture memory (%u KB in use)\n", mTextureBytes >> 10);
			return missingTexture();
		}
	}
	mStaging.resize(k.width * k.height);
	gpu::decodeTexture(src, k.width, k.height, k.format, mTmem, tlut, k.tlutFormat, &mStaging[0], k.width, gpu::TEXELS_ARGB);
	for (u32 y = 0; y < k.height; y++) {
		memcpy((u8*)t->texels + y * t->pitch, &mStaging[y * k.width], k.width * 4);
	}
	mDev.texturesChanged();
	mTextureBytes += t->bytes;
	mStats.textureUploads++;
	mStats.textureUploadBytes += t->bytes;
	retire(e.tex);
	e.tex          = t;
	e.hash         = hash;
	e.checkedFrame = mFrame;
	e.usedFrame    = mFrame;
	return t;
}

// Works out the device state, program and textures for the current registers. False: nothing to draw.
bool Renderer::updateState()
{
	const gpu::State& S = mCp.state();
	const u32* bp       = S.bp;
	const u32* xf       = S.xf;
	if (memcmp(mViewport, xf + 0x101A, sizeof(mViewport)) != 0) {
		memcpy(mViewport, xf + 0x101A, sizeof(mViewport));
		mDirty = true;
	}
	if (!mDirty) {
		return mDrawable;
	}
	mDirty    = false;
	mDrawable = false;

	// ---- device state: blending, depth, color mask, scissor, viewport
	RenderState rs;
	memset(&rs, 0, sizeof(rs));
	const u32 zmode = bp[0x40], cm0 = bp[0x41], pe = bp[0x43];
	const bool efbAlpha = bits(pe, 0, 3) == 1; // RGBA6_Z24: the frame buffer keeps alpha
	if (bits(cm0, 11, 1)) {
		rs.blend = 2;
		rs.srcFactor = BLEND_ONE, rs.dstFactor = BLEND_ONE;
	} else if (bits(cm0, 0, 1)) {
		rs.blend     = 1;
		rs.srcFactor = kBlendSrc[bits(cm0, 8, 3)];
		rs.dstFactor = kBlendDst[bits(cm0, 5, 3)];
	} else if (bits(cm0, 1, 1)) {
		rs.logicEnable = 1;
		rs.logicOp     = (u8)bits(cm0, 12, 4);
	}
	rs.depthTest  = (u8)bits(zmode, 0, 1);
	rs.depthFunc  = (u8)bits(zmode, 1, 3);
	rs.depthWrite = (u8)(bits(zmode, 0, 1) && bits(zmode, 4, 1));
	rs.colorWrite = (u8)bits(cm0, 3, 1);
	rs.alphaWrite = (u8)(efbAlpha && bits(cm0, 4, 1));
	int sx0 = (int)bits(bp[0x20], 12, 11) - 342, sy0 = (int)bits(bp[0x20], 0, 11) - 342;
	int sx1 = (int)bits(bp[0x21], 12, 11) - 342, sy1 = (int)bits(bp[0x21], 0, 11) - 342;
	sx0 = clampi(sx0, 0, EFB_WIDTH), sy0 = clampi(sy0, 0, EFB_HEIGHT);
	sx1 = clampi(sx1 + 1, 0, EFB_WIDTH), sy1 = clampi(sy1 + 1, 0, EFB_HEIGHT);
	if (sx1 <= sx0 || sy1 <= sy0) {
		return false;
	}
	rs.scissor[0] = (u16)sx0, rs.scissor[1] = (u16)sy0;
	rs.scissor[2] = (u16)(sx1 - sx0), rs.scissor[3] = (u16)(sy1 - sy0);
	rs.viewportScale[0]  = asFloat(xf[0x101A]);
	rs.viewportScale[1]  = asFloat(xf[0x101B]);
	rs.viewportScale[2]  = asFloat(xf[0x101C]) / 16777215.0f;
	rs.viewportOffset[0] = asFloat(xf[0x101D]) - 342.0f;
	rs.viewportOffset[1] = asFloat(xf[0x101E]) - 342.0f;
	rs.viewportOffset[2] = asFloat(xf[0x101F]) / 16777215.0f;
	if (!mStateValid || rs != mState) {
		flushBatch();
		mDev.setState(rs);
		mState      = rs;
		mStateValid = true;
	}
	mCull = bits(bp[0x00], 14, 2);
	if (mCull == 3) {
		return false;
	}

	// ---- fragment program for the TEV set-up
	TevKey key;
	makeTevKey(bp, key);
	if (!mProgram || !(mProgram->key == key)) {
		std::vector<ProgramEntry*>& chain = mPrograms[key.hash()];
		ProgramEntry* found               = NULL;
		for (size_t i = 0; i < chain.size(); i++) {
			if (chain[i]->key == key) {
				found = chain[i];
				break;
			}
		}
		if (!found) {
			found      = new ProgramEntry;
			found->key = key;
			const char* error;
			found->ok = buildTevProgram(key, found->prog, &error);
			mStats.programs++;
			if (!found->ok) {
				mStats.programErrors++;
				if (mStats.programErrors <= 3) {
					log("[rsx] FAILED to assemble a TEV program (%s):\n", error ? error : "?");
					const std::string& t = found->prog.text;
					for (size_t at = 0; at < t.size(); at += 400) {
						log("%s", t.substr(at, 400).c_str());
					}
					log("\n");
				}
			}
			chain.push_back(found);
		}
		if (found != mProgram) {
			mProgram      = found;
			mProgramBound = false;
		}
	}
	if (!mProgram->ok || mProgram->prog.alwaysKill) {
		return false;
	}

	// parameters: TEV registers, constants, alpha test references
	float params[NUM_TEV_PARAMS][4];
	for (int r = 0; r < 4; r++) {
		for (int k = 0; k < 4; k++) {
			params[r][k]     = mTevReg[r][k] / 255.0f;
			params[4 + r][k] = mKonst[r][k] / 255.0f;
		}
	}
	params[8][0] = (float)bits(bp[0xF3], 0, 8);
	params[8][1] = (float)bits(bp[0xF3], 8, 8);
	params[8][2] = params[8][3] = 0;
	const u32 used = mProgram->prog.params;
	for (u32 i = 0; i < NUM_TEV_PARAMS; i++) {
		if ((used & (1u << i)) && memcmp(params[i], mParams[i], sizeof(params[i])) != 0) {
			memcpy(mParams[i], params[i], sizeof(params[i]));
			mProgramBound = false;
		}
	}
	if (!mProgramBound) {
		flushBatch();
		bindProgram(mProgram->prog.fp, mParams);
		mProgramBound = true;
	}

	// ---- textures
	for (u32 m = 0; m < 8; m++) {
		if (!(mProgram->prog.texMaps & (1u << m))) {
			continue;
		}
		Sampler s;
		const Texture* t = lookupTexture(m, s);
		if (!mBound[m].valid || mBound[m].tex != t || !(mBound[m].sampler == s)) {
			flushBatch();
			mDev.setTexture(m, t, s);
			mBound[m].tex     = t;
			mBound[m].sampler = s;
			mBound[m].valid   = true;
		}
	}
	mDrawable = true;
	return true;
}

void Renderer::bpWritten(u32 reg, u32 value)
{
	if (reg == 0x45 || reg == 0x47 || reg == 0x48 || reg == 0x52 || reg == 0xFE) {
		return; // (synchronization and copies)
	}
	mDirty = true;
	if (reg >= 0xE0 && reg < 0xE8) {
		tevRegFromBp(mTevReg, mKonst, reg, value);
	} else if (reg == 0x65) {
		// TLUT load: from memory (BP 0x64) into texture memory
		const u32* bp = mCp.state().bp;
		u32 address   = bits(bp[0x64], 0, 21) << 5;
		u32 tmem      = bits(value, 0, 10) << 9;
		u32 bytes     = bits(value, 10, 11) * 32;
		const u8* src = bytes ? mMem.read(address, bytes, gpu::MEM_TLUT) : NULL;
		if (src && tmem + bytes <= 0x100000) {
			memcpy(mTmem + tmem, src, bytes);
		}
	}
}

// ================================================================== draws

void Renderer::draw(const gpu::DrawCall& call)
{
	mStats.draws++;
	if (!call.vertices || !call.count) {
		return;
	}
	const u32 prim = call.primitive;
	if (prim == gpu::PRIM_LINES || prim == gpu::PRIM_LINE_STRIP) {
		mStats.lineDraws++;
		return;
	}
	if (prim == gpu::PRIM_POINTS) {
		mStats.pointDraws++;
		return;
	}
	if (!updateState()) {
		mStats.skippedDraws++;
		return;
	}
	const gpu::State& S = mCp.state();
	u32 texCoords       = 0;
	for (u32 t = 0; t < 8; t++) {
		if (mProgram->prog.texCoords & (1u << t)) {
			texCoords = t + 1;
		}
	}
	u32 first;
	u8* out = batchVertices(call.count, texCoords, &first);
	if (!out) {
		mStats.skippedDraws++;
		return;
	}
	const u32 stride = mBatch.stride;

	// (XF memory changes only with XF loads: the set-up is kept while there are none)
	const gpu::Stats& cs = mCp.stats();
	const u64 xfSerial   = cs.xfLoads + cs.indexedLoads;
	if (!mXfValid || xfSerial != mXfSerial || memcmp(&mXfLayout, call.layout, sizeof(mXfLayout)) != 0) {
		mXfLayout = *call.layout;
		gpu::xfPrepare(S.xf, mXfLayout, mXf);
		mXfSerial = xfSerial;
		mXfValid  = true;
	}
	gpu::xfLimit(mXf, mProgram->prog.colorChans, texCoords);
	const gpu::XfSetup& xs = mXf;
	gpu::XfVertex v;
	for (u32 i = 0; i < call.count; i++) {
		gpu::xfTransform(xs, call.vertices[i], v);
		u8* o    = out + i * stride;
		float* f = (float*)o;
		f[0] = v.pos[0], f[1] = v.pos[1], f[2] = v.pos[2], f[3] = v.pos[3];
		o[16] = colorByte(v.color[0][0]), o[17] = colorByte(v.color[0][1]);
		o[18] = colorByte(v.color[0][2]), o[19] = colorByte(v.color[0][3]);
		o[20] = colorByte(v.color[1][0]), o[21] = colorByte(v.color[1][1]);
		o[22] = colorByte(v.color[1][2]), o[23] = colorByte(v.color[1][3]);
		float* tc = (float*)(o + 24);
		for (u32 t = 0; t < texCoords; t++) {
			tc[t * 3] = v.tex[t][0], tc[t * 3 + 1] = v.tex[t][1], tc[t * 3 + 2] = v.tex[t][2];
		}
	}
	mStats.vertices += call.count;

	// Triangles, without those facing away (sign of the screen-space area, from the homogeneous positions:
	// correct also for triangles that cross the camera plane).
	const float xyScale = mState.viewportScale[0] * mState.viewportScale[1];
	const u32 cull      = mCull;
	std::vector<unsigned short>& ix = mBatch.indices;
	const u32 n = call.count;
#define POS(i) ((const float*)(out + (i) * stride))
#define TRI(i0, i1, i2)                                                                                                  \
	do {                                                                                                                 \
		mStats.triangles++;                                                                                              \
		if (cull) {                                                                                                      \
			const float* a = POS(i0);                                                                                    \
			const float* b = POS(i1);                                                                                    \
			const float* c = POS(i2);                                                                                    \
			float det = a[0] * (b[1] * c[3] - c[1] * b[3]) - a[1] * (b[0] * c[3] - c[0] * b[3]) + a[3] * (b[0] * c[1] - c[0] * b[1]); \
			det *= xyScale;                                                                                              \
			if ((cull == 1 && det < 0) || (cull == 2 && det > 0)) {                                                      \
				mStats.culled++;                                                                                         \
				break;                                                                                                   \
			}                                                                                                            \
		}                                                                                                                \
		ix.push_back((unsigned short)(first + (i0)));                                                                    \
		ix.push_back((unsigned short)(first + (i1)));                                                                    \
		ix.push_back((unsigned short)(first + (i2)));                                                                    \
	} while (0)

	switch (prim) {
	case gpu::PRIM_QUADS:
	case gpu::PRIM_QUADS_2:
		for (u32 i = 0; i + 3 < n; i += 4) {
			TRI(i, i + 1, i + 2);
			TRI(i, i + 2, i + 3);
		}
		break;
	case gpu::PRIM_TRIANGLES:
		for (u32 i = 0; i + 2 < n; i += 3) {
			TRI(i, i + 1, i + 2);
		}
		break;
	case gpu::PRIM_TRI_STRIP:
		for (u32 i = 0; i + 2 < n; i++) {
			if (i & 1) {
				TRI(i + 1, i, i + 2);
			} else {
				TRI(i, i + 1, i + 2);
			}
		}
		break;
	case gpu::PRIM_TRI_FAN:
		for (u32 i = 1; i + 1 < n; i++) {
			TRI(0, i, i + 1);
		}
		break;
	default: break;
	}
#undef TRI
#undef POS
	mBatch.vertexCount += call.count;
}

// ================================================================== copies out of the frame buffer

void Renderer::drawQuad(u32 tw, u32 th, const float dst[4], const float uv[4], const Texture* source, bool linear,
                        const FragmentProgram& fp)
{
	flushBatch();
	RenderState rs;
	memset(&rs, 0, sizeof(rs));
	rs.colorWrite = rs.alphaWrite = 1;
	rs.scissor[0] = rs.scissor[1] = 0;
	rs.scissor[2] = (u16)tw, rs.scissor[3] = (u16)th;
	rs.viewportScale[0] = tw * 0.5f, rs.viewportScale[1] = th * -0.5f, rs.viewportScale[2] = 0.5f;
	rs.viewportOffset[0] = tw * 0.5f, rs.viewportOffset[1] = th * 0.5f, rs.viewportOffset[2] = 0.5f;
	mDev.setState(rs);
	bindProgram(fp, NULL);
	Sampler s;
	s.wrapS = s.wrapT = WRAP_CLAMP;
	s.linearMin = s.linearMag = linear ? 1 : 0;
	mDev.setTexture(0, source, s);

	u32 first;
	u8* out = batchVertices(4, 1, &first);
	if (!out) {
		return;
	}
	const float x[4] = { dst[0], dst[2], dst[2], dst[0] };
	const float y[4] = { dst[1], dst[1], dst[3], dst[3] };
	const float u[4] = { uv[0], uv[2], uv[2], uv[0] };
	const float v[4] = { uv[1], uv[1], uv[3], uv[3] };
	for (int i = 0; i < 4; i++) {
		u8* o    = out + i * mBatch.stride;
		float* f = (float*)o;
		f[0] = x[i] / tw * 2.0f - 1.0f;
		f[1] = 1.0f - y[i] / th * 2.0f;
		f[2] = 0.0f;
		f[3] = 1.0f;
		memset(o + 16, 255, 8);
		float* tc = (float*)(o + 24);
		tc[0] = u[i], tc[1] = v[i], tc[2] = 1.0f;
	}
	static const unsigned short quad[6] = { 0, 1, 2, 0, 2, 3 };
	for (int i = 0; i < 6; i++) {
		mBatch.indices.push_back((unsigned short)(first + quad[i]));
	}
	mBatch.vertexCount += 4;
	flushBatch();
	invalidateDeviceState();
}

void Renderer::present(const gpu::CopyInfo& info)
{
	if (!mCopyProgramReady[COPY_RGB][0]) {
		const char* error;
		std::string text = copyProgramText(COPY_RGB, false);
		if (!assembleFragmentProgram(text.c_str(), mCopyPrograms[COPY_RGB][0], &error)) {
			log("[rsx] FAILED to assemble the display program: %s\n", error);
			return;
		}
		mCopyProgramReady[COPY_RGB][0] = true;
	}
	u32 dw, dh;
	if (!mDev.beginDisplay(&dw, &dh)) {
		mDev.setTarget(NULL);
		return;
	}
	// the frame, 4:3, as large as fits, centered
	float h = (float)dh, w = h * 4.0f / 3.0f;
	if (w > dw) {
		w = (float)dw, h = w * 3.0f / 4.0f;
	}
	const float x0 = (dw - w) * 0.5f, y0 = (dh - h) * 0.5f;
	const u16 all[4] = { 0, 0, (u16)dw, (u16)dh };
	mDev.clear(all, 0xFF000000, 0, true, true, false);
	mDev.texturesChanged(); // (the frame buffer is read as a texture)
	const float dst[4] = { x0, y0, x0 + w, y0 + h };
	const float uv[4]  = { (float)info.srcLeft / EFB_WIDTH, (float)info.srcTop / EFB_HEIGHT,
		                   (float)(info.srcLeft + info.width) / EFB_WIDTH, (float)(info.srcTop + info.height) / EFB_HEIGHT };
	drawQuad(dw, dh, dst, uv, mDev.efbColor(), true, mCopyPrograms[COPY_RGB][0]);
	mDev.endDisplay();
	mDev.setTarget(NULL);
	invalidateDeviceState();
	mStats.presents++;
}

void Renderer::copyToTexture(const gpu::CopyInfo& info)
{
	mStats.copies++;
	const u32 field    = bits(info.control, 3, 4);
	const u32 format   = field / 2 + (field & 1) * 8; // (the texture format field's bits are rotated)
	const bool half    = bits(info.control, 9, 1);
	const bool intens  = bits(info.control, 15, 1);
	const u32 w        = half ? info.width / 2 : info.width;
	const u32 h        = half ? info.height / 2 : info.height;
	if (!w || !h) {
		return;
	}
	CopyMode mode;
	switch (format) {
	case 0:
	case 1:
	case 8: mode = COPY_R_ALL; break;
	case 2:
	case 3: mode = COPY_RA; break;
	case 4: mode = COPY_RGB; break;
	case 7: mode = COPY_A_ALL; break;
	case 9: mode = COPY_G_ALL; break;
	case 10: mode = COPY_B_ALL; break;
	default: mode = COPY_RGBA; break;
	}
	const int in = (intens && (mode == COPY_R_ALL || mode == COPY_RA)) ? 1 : 0;
	if (!mCopyProgramReady[mode][in]) {
		const char* error;
		std::string text = copyProgramText(mode, in != 0);
		if (!assembleFragmentProgram(text.c_str(), mCopyPrograms[mode][in], &error)) {
			log("[rsx] FAILED to assemble a copy program: %s\n", error);
			return;
		}
		mCopyProgramReady[mode][in] = true;
	}
	Texture* t = mDev.createTexture(w, h);
	if (!t) {
		evictTextures();
		release();
		t = mDev.createTexture(w, h);
		if (!t) {
			log("[rsx] out of memory for a frame buffer copy (%ux%u)\n", w, h);
			return;
		}
	}
	mTextureBytes += t->bytes;
	mDev.setTarget(t);
	mDev.texturesChanged(); // (the frame buffer is read as a texture)
	const float dst[4] = { 0, 0, (float)w, (float)h };
	const float uv[4]  = { (float)info.srcLeft / EFB_WIDTH, (float)info.srcTop / EFB_HEIGHT,
		                   (float)(info.srcLeft + info.width) / EFB_WIDTH, (float)(info.srcTop + info.height) / EFB_HEIGHT };
	drawQuad(w, h, dst, uv, mDev.efbColor(), half, mCopyPrograms[mode][in]);
	mDev.setTarget(NULL);
	mDev.texturesChanged();
	invalidateDeviceState();

	const u32 address = info.dest & 0x3FFFFE0;
	CopyEntry& c      = mCopies[address];
	retire(c.tex);
	c.tex          = t;
	c.width        = w;
	c.height       = h;
	c.format       = format;
	c.checkedFrame = mFrame;
	u32 size       = gpu::CommandProcessor::textureSize(format, w, h, 1);
	const u8* mem  = mMem.read(address, size, gpu::MEM_TEXTURE);
	c.memKnown     = mem != NULL;
	c.memHash      = mem ? gpu::hashMemory(mem, size) : 0;
	// (decoded textures at that address are out of date)
	for (std::map<TexKey, TexEntry>::iterator it = mTextures.begin(); it != mTextures.end();) {
		if (it->first.address == address) {
			retire(it->second.tex);
			mTextures.erase(it++);
		} else {
			++it;
		}
	}
}

void Renderer::clearEfb(const gpu::CopyInfo& info)
{
	const u32* bp = mCp.state().bp;
	const u32 ar = bp[0x4F], gb = bp[0x50];
	const u32 cm0 = bp[0x41];
	const bool efbAlpha = bits(bp[0x43], 0, 3) == 1;
	bool color = bits(cm0, 3, 1), alpha = bits(cm0, 4, 1);
	const bool depth = bits(bp[0x40], 4, 1);
	u32 a = bits(ar, 8, 8);
	if (!efbAlpha) {
		a     = 255; // (formats without alpha read as opaque: keep the stored alpha at 1)
		alpha = color;
	}
	const u32 argb = (a << 24) | (bits(ar, 0, 8) << 16) | (bits(gb, 8, 8) << 8) | bits(gb, 0, 8);
	int x0 = clampi((int)info.srcLeft, 0, EFB_WIDTH), y0 = clampi((int)info.srcTop, 0, EFB_HEIGHT);
	int x1 = clampi((int)(info.srcLeft + info.width), 0, EFB_WIDTH), y1 = clampi((int)(info.srcTop + info.height), 0, EFB_HEIGHT);
	if (x1 <= x0 || y1 <= y0 || !(color || alpha || depth)) {
		return;
	}
	const u16 rect[4] = { (u16)x0, (u16)y0, (u16)(x1 - x0), (u16)(y1 - y0) };
	mDev.clear(rect, argb, bp[0x51] & 0xFFFFFF, color, alpha, depth);
	mStats.clears++;
}

void Renderer::copy(const gpu::CopyInfo& info)
{
	flushBatch();
	if (info.toXfb) {
		present(info);
	} else {
		copyToTexture(info);
	}
	if (info.clear) {
		clearEfb(info);
	}
	if (info.toXfb) {
		endFrame();
	}
}

void Renderer::endFrame()
{
	mFrame++;
	mDirty = true;
	mark();
	mDev.kick();
	release();
	evictTextures();
	mStats.textureBytes = mTextureBytes;
	mStats.textures     = (u32)mTextures.size();
}

// Textures not used in the last two frames go when the decoded textures take more than the budget.
void Renderer::evictTextures()
{
	if (mTextureBytes <= TEXTURE_BUDGET) {
		return;
	}
	for (std::map<TexKey, TexEntry>::iterator it = mTextures.begin(); it != mTextures.end() && mTextureBytes > TEXTURE_BUDGET * 3 / 4;) {
		if (it->second.usedFrame + 2 <= mFrame) {
			retire(it->second.tex);
			mTextures.erase(it++);
		} else {
			++it;
		}
	}
}

} // namespace gfx
