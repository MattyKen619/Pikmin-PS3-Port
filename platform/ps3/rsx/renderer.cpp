// PS3 port: the GameCube's graphics commands drawn through a Device (see renderer.h).
//
// Register layouts follow the hardware as the GX library (src/gx) programs it; the reference for every
// rule here is the host tools' software renderer (tools/ps3/gx_host/sw_render.cpp).

#include "renderer.h"

#include "../gpu/gpu_tex.h"

#include <malloc.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RSX_VERTEX_CACHE_BYTES
#define RSX_VERTEX_CACHE_BYTES (8 * 1024 * 1024)
#endif
#ifndef RSX_AUDIT_FRAMES
#define RSX_AUDIT_FRAMES 300
#endif

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

enum {
	BATCH_RESERVE   = 256 * 1024,
	TEXTURE_BUDGET  = 96 * 1024 * 1024, // device memory for decoded textures before old ones are evicted
	MAX_BATCH_VERTS = 65535,
	TEXTURE_CHECK_FRAMES = 8,
	ARENA_BYTES     = RSX_VERTEX_CACHE_BYTES, // vertex cache memory (at most a third of the scratch memory)
	CACHE_ENTRIES   = 32768,           // entries before the vertex cache starts over
	POOL_CHUNK      = 1024 * 1024,
	POOL_MAX        = 12 * 1024 * 1024, // CPU memory for the entries before the vertex cache starts over
	AUDIT_FRAMES    = RSX_AUDIT_FRAMES, // one frame in this many is an audit frame (Renderer::mAudit)
	AUDIT_LOGS      = 40,              // log lines about what an audit found, at most
};

// The triangles of a draw's primitive as indices (first: the index of its first vertex). Returns the count.
template <typename I>
u32 triangleIndices(u32 prim, u32 n, u32 first, I* ix)
{
	I* const start = ix;
	switch (prim) {
	case gpu::PRIM_QUADS:
	case gpu::PRIM_QUADS_2:
		for (u32 i = 0; i + 3 < n; i += 4, ix += 6) {
			ix[0] = (I)(first + i), ix[1] = (I)(first + i + 1), ix[2] = (I)(first + i + 2);
			ix[3] = (I)(first + i), ix[4] = (I)(first + i + 2), ix[5] = (I)(first + i + 3);
		}
		break;
	case gpu::PRIM_TRIANGLES:
		for (u32 i = 0; i + 2 < n; i += 3, ix += 3) {
			ix[0] = (I)(first + i), ix[1] = (I)(first + i + 1), ix[2] = (I)(first + i + 2);
		}
		break;
	case gpu::PRIM_TRI_STRIP:
		for (u32 i = 0; i + 2 < n; i++, ix += 3) {
			if (i & 1) {
				ix[0] = (I)(first + i + 1), ix[1] = (I)(first + i);
			} else {
				ix[0] = (I)(first + i), ix[1] = (I)(first + i + 1);
			}
			ix[2] = (I)(first + i + 2);
		}
		break;
	case gpu::PRIM_TRI_FAN:
		for (u32 i = 1; i + 1 < n; i++, ix += 3) {
			ix[0] = (I)first, ix[1] = (I)(first + i), ix[2] = (I)(first + i + 1);
		}
		break;
	default: break;
	}
	return (u32)(ix - start);
}

u32 triangleCount(u32 prim, u32 n)
{
	switch (prim) {
	case gpu::PRIM_QUADS:
	case gpu::PRIM_QUADS_2: return (n / 4) * 2;
	case gpu::PRIM_TRIANGLES: return n / 3;
	case gpu::PRIM_TRI_STRIP:
	case gpu::PRIM_TRI_FAN: return n >= 3 ? n - 2 : 0;
	default: return 0;
	}
}

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

// Which parts of the state a BP register affects (updateState redoes only those).
u8 sBpDirty[256];
bool sBpDirtyReady;

void initBpDirty()
{
	enum { D_STATE = Renderer::D_STATE, D_TEV = Renderer::D_TEV, D_PARAMS = Renderer::D_PARAMS, D_TEX = Renderer::D_TEX };
	if (sBpDirtyReady) {
		return;
	}
	sBpDirty[0x00] = D_STATE | D_TEV;                       // gen mode: stages, culling
	sBpDirty[0x20] = sBpDirty[0x21] = sBpDirty[0x59] = D_STATE; // scissor
	for (u32 r = 0x40; r <= 0x43; r++) sBpDirty[r] = D_STATE; // depth, blending, pixel format
	for (u32 r = 0x28; r <= 0x2F; r++) sBpDirty[r] = D_TEV | D_TEX; // stage orders (which maps)
	for (u32 r = 0xC0; r <= 0xDF; r++) sBpDirty[r] = D_TEV; // combiners
	for (u32 r = 0xF6; r <= 0xFD; r++) sBpDirty[r] = D_TEV; // constant selections, swap tables
	sBpDirty[0xF3] = D_TEV | D_PARAMS;                       // alpha test
	for (u32 r = 0xE0; r <= 0xE7; r++) sBpDirty[r] = D_PARAMS; // TEV registers
	for (u32 r = 0x80; r <= 0x9B; r++) sBpDirty[r] = D_TEX; // texture maps 0-3
	for (u32 r = 0xA0; r <= 0xBB; r++) sBpDirty[r] = D_TEX; // texture maps 4-7
	sBpDirty[0x64] = sBpDirty[0x65] = D_TEX;                 // palettes
	sBpDirtyReady = true;
}

} // namespace

// Texture memory is checked for changes (hashed) at most once a frame: every frame while it keeps
// changing (a movie), then less and less often while it does not, down to once in TEXTURE_CHECK_FRAMES
// frames. (Test 24 checked every texture in its own fixed frame of an 8-frame cycle: a texture used only
// in the other frames, like a movie's alternating images, was never checked again.)
void Renderer::checked(u32& checkedFrame, u8& interval, bool changed, u32 address) const
{
	if (changed) {
		interval = 1;
	} else {
		interval = (u8)(interval * 2 > TEXTURE_CHECK_FRAMES ? TEXTURE_CHECK_FRAMES : (interval ? interval * 2 : 2));
	}
	checkedFrame = mFrame;
	if (interval >= TEXTURE_CHECK_FRAMES && !mCheckEveryFrame) {
		checkedFrame -= (address >> 5) & 3; // (textures checked together drift apart: the work is spread out)
	}
}

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
    , mFrameFence(0)
    , mDirtyBits(D_ALL)
    , mDrawable(false)
    , mStateOk(false)
    , mStateValid(false)
    , mCull(0)
    , mProgram(NULL)
    , mProgramBound(false)
    , mMissing(NULL)
    , mTextureBytes(0)
    , mXfSerial(0)
    , mXfValid(false)
    , mGpuXf(false)
    , mXfLookup(NULL)
    , mXfLookupValid(false)
    , mXfLookupFp(NULL)
    , mXfLookupTexCoords(0)
    , mXfLayoutAt(NULL)
    , mXfLayoutVersion(0)
    , mXfLimitFp(NULL)
    , mBoundXp(NULL)
    , mBoundXpValid(false)
    , mSetupValid(false)
    , mSetupDirty(true)
    , mXfWrites(1)
    , mCacheOn(false)
    , mResetRequest(0)
    , mSwitchRequest(0)
    , mMode(0)
    , mCheckEveryFrame(false)
    , mFrontFaceSign(0)
    , mLastEntry(NULL)
    , mAudit(false)
    , mChainLength(0)
    , mArena(NULL)
    , mArenaSize(0)
    , mArenaHead(0)
    , mArenaGen(1)
    , mIdx(NULL)
    , mIdxSize(0)
    , mIdxHead(0)
    , mTable(NULL)
    , mTableSlots(0)
    , mTableUsed(0)
    , mPoolUsed(0)
    , mPoolBytes(0)
    , mRegions(NULL)
{
	// (every draw from the vertex cache reads its entry: one cache line)
	static_assert(sizeof(CacheEntry) <= 128 && sizeof(CacheEntry) % 8 == 0, "a vertex cache entry is one cache line (and its copy of the bytes as aligned as the display list's)");
	memset(mSetup, 0, sizeof(mSetup));
	memset(&mAuditCounts, 0, sizeof(mAuditCounts));
	mStale.lo = mNStale.lo = ~0ULL;
	mStale.hi = mNStale.hi = 3;
	for (int i = 0; i < 64; i++) {
		mIdentity[i] = (u8)i;
	}
	memset(&mStats, 0, sizeof(mStats));
	memset(mTevReg, 0, sizeof(mTevReg));
	memset(mKonst, 0, sizeof(mKonst));
	memset(mViewport, 0, sizeof(mViewport));
	memset(mParams, 0, sizeof(mParams));
	memset(mRegF, 0, sizeof(mRegF));
	memset(mTlutCache, 0, sizeof(mTlutCache));
	mTlutSerial = 1;
	memset(&mState, 0, sizeof(mState));
	initBpDirty();
	mTmem = (u8*)calloc(1, 0x100000);
	mScratch     = mDev.scratchBase();
	mScratchSize = mDev.scratchSize();
	// the vertex cache's memory: the end of the scratch memory (the ring keeps the rest), 3/4 for vertices
	// and 1/4 for their triangles
	u32 cacheBytes = (mScratchSize / 3) & ~127u;
	if (cacheBytes > (u32)ARENA_BYTES) {
		cacheBytes = ARENA_BYTES;
	}
	mScratchSize = (mScratchSize - cacheBytes) & ~127u;
	mArena       = mScratch + mScratchSize;
	mArenaSize   = (cacheBytes / 4 * 3) & ~127u;
	mIdx         = (u32*)(mArena + mArenaSize);
	mIdxSize     = (cacheBytes - mArenaSize) / 4;
	mBatch.active = false;
	mBatch.cached = false;
	mBatch.base   = NULL;
	mBatch.reserved = mBatch.stride = mBatch.texCoords = mBatch.vertexCount = 0;
	mBatch.xp       = NULL;
	mBatch.rows.lo = mBatch.nrows.lo = 0;
	mBatch.rows.hi = mBatch.nrows.hi = 0;
	mBatch.at32 = mBatch.count32 = 0;
	for (int i = 0; i < 8; i++) {
		mBound[i].valid = false;
		mBound[i].tex   = NULL;
	}
	memset(mCopyProgramReady, 0, sizeof(mCopyProgramReady));
	mTableSlots = 1 << 16;
	mTable      = (CacheEntry**)calloc(mTableSlots, sizeof(CacheEntry*));
	setGpuTransform(true);
}

void Renderer::setGpuTransform(bool on)
{
	flushBatch();
	mFrontFaceSign = mDev.frontFaceSign();
	mGpuXf         = on && mDev.vertexProgramsWork() && mFrontFaceSign != 0 && mDev.vertexIndexType() != 0;
	mCacheOn       = mGpuXf && mDev.index32Works() && mTable && mArenaSize >= 1024 * 1024;
}

void Renderer::setVertexCache(bool on)
{
	flushBatch();
	mCacheOn = on && mGpuXf && mDev.index32Works() && mTable && mArenaSize >= 1024 * 1024;
}

Renderer::~Renderer()
{
	free(mTmem);
	free(mTable);
	for (size_t i = 0; i < mPool.size(); i++) {
		free(mPool[i]);
	}
	for (Map<u32, Vec<ProgramEntry*> >::iterator it = mPrograms.begin(); it != mPrograms.end(); ++it) {
		for (size_t i = 0; i < it->second.size(); i++) {
			destroy(it->second[i]);
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
		bpWritten(r, S.bp[r]);
	}
	bpWritten(0xF3, S.bp[0xF3]);
	mDirtyBits = D_ALL;
	mXfValid   = false;
	mXfLookupValid = false;
	mXfLimitFp     = NULL;
	mSetupDirty    = true;
	// (XF memory came from somewhere else: no row of the copy in the constants is known to be current)
	flushBatch();
	mStale.lo = mNStale.lo = ~0ULL;
	mStale.hi = mNStale.hi = 3;
}

void Renderer::flush()
{
	flushBatch();
	mark();
	mDev.kick();
}

bool Renderer::readEfb(u32* out, u32 x, u32 y, u32 width, u32 height)
{
	gpu::PhaseScope wait(gpu::GFX_WAIT_RSX);
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
		gpu::PhaseScope wait(gpu::GFX_WAIT_RSX);
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
			mDirtyBits |= D_TEX;
		}
	}
	Retired r;
	r.tex   = t;
	r.fence = mFenceCounter + 1;
	mRetired.push_back(r);
}

// ================================================================== batches

u8* Renderer::batchVertices(u32 count, u32 stride, u32 texCoords, XfProgramEntry* xp, u32* first)
{
	if (mBatch.active
	    && (mBatch.cached || xp != mBatch.xp || stride != mBatch.stride || texCoords != mBatch.texCoords
	        || mBatch.vertexCount + count > MAX_BATCH_VERTS || (mBatch.vertexCount + count) * stride > mBatch.reserved)) {
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
		mBatch.xp       = xp;
		mBatch.vertexCount = 0;
		mBatch.indices.clear();
		mBatch.active = true;
		mBatch.cached = false;
	}
	*first = mBatch.vertexCount;
	return mBatch.base + mBatch.vertexCount * stride;
}

// Draws from the vertex cache go into a batch of their own kind: all of the cache memory is one vertex
// array per vertex format (32-bit indices), so draws with the same program join whatever their position.
void Renderer::batchCached(CacheEntry* e, XfProgramEntry* xp)
{
	if (mBatch.active && (!mBatch.cached || xp != mBatch.xp || e->indexAt != mBatch.at32 + mBatch.count32)) {
		flushBatch();
	}
	if (!mBatch.active) {
		mBatch.active      = true;
		mBatch.cached      = true;
		mBatch.base        = mArena;
		mBatch.reserved    = 0;
		mBatch.stride      = xp->info.format.stride;
		mBatch.texCoords   = 0;
		mBatch.xp          = xp;
		mBatch.vertexCount = 0;
		mBatch.at32        = e->indexAt;
		mBatch.count32     = 0;
	}
	mBatch.count32 += e->indexCount;
	mBatch.vertexCount += e->count;
}

void Renderer::flushBatch()
{
	if (!mBatch.active) {
		return;
	}
	if (mBatch.cached) {
		if (mBatch.count32) {
			gpu::PhaseScope phase(gpu::GFX_RSX);
			if (mBatch.xp != mBoundXp || !mBoundXpValid) {
				mStats.vertexProgramLoads++;
				mBoundXp      = mBatch.xp;
				mBoundXpValid = true;
			}
			mDev.setVertexProgram(&mBatch.xp->prog);
			mDev.setVertexFormat(mArena, mBatch.xp->info.format);
			if (mMode == MODE_INLINE) {
				mDev.drawTriangles32Inline(mIdx + mBatch.at32, mBatch.count32);
			} else {
				mDev.drawTriangles32(mIdx + mBatch.at32, mBatch.count32);
			}
			mStats.batches++;
			if ((mStats.batches & 31) == 0) {
				mDev.kick(); // (the RSX starts on them while more are made)
			}
		}
		mBatch.active  = false;
		mBatch.cached  = false;
		mBatch.count32 = 0;
		mBatch.rows.lo = mBatch.nrows.lo = 0;
		mBatch.rows.hi = mBatch.nrows.hi = 0;
		return;
	}
	// The unused part of the reservation goes back to the ring (nothing was allocated after it).
	mHead -= mBatch.reserved - mBatch.vertexCount * mBatch.stride;
	if (!mBatch.indices.empty()) {
		gpu::PhaseScope phase(gpu::GFX_RSX);
		if (mBatch.xp != mBoundXp || !mBoundXpValid) {
			mStats.vertexProgramLoads++;
			mBoundXp      = mBatch.xp;
			mBoundXpValid = true;
		}
		if (mBatch.xp) {
			mDev.setVertexProgram(&mBatch.xp->prog);
			mDev.setVertexFormat(mBatch.base, mBatch.xp->info.format);
		} else {
			mDev.setVertexProgram(NULL);
			mDev.setVertices(mBatch.base, mBatch.stride, mBatch.texCoords);
		}
		mDev.drawTriangles(&mBatch.indices[0], (u32)mBatch.indices.size());
		mStats.batches++;
	}
	mBatch.active = false;
	mBatch.indices.clear();
	mBatch.rows.lo = mBatch.nrows.lo = 0;
	mBatch.rows.hi = mBatch.nrows.hi = 0;
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
	mDirtyBits = D_ALL;
}

void Renderer::bindProgram(const FragmentProgram& fp, const float (*params)[4])
{
	gpu::PhaseScope phase(gpu::GFX_RSX);
	const u32 bytes = (u32)fp.ucode.size() * 4;
	u32* dst        = mMode == MODE_SPACED ? (u32*)ringAlloc((bytes + 4095) & ~4095u, 4096) : (u32*)ringAlloc(bytes, 128);
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
	Map<u32, CopyEntry>::iterator ci = mCopies.find(k.address);
	if (ci != mCopies.end()) {
		CopyEntry& c = ci->second;
		bool valid   = true;
		if (c.memKnown && checkDue(c.checkedFrame, c.interval)) {
			// (the game may have loaded something else there since)
			gpu::PhaseScope phase(gpu::GFX_CHECKS);
			u32 size      = gpu::CommandProcessor::textureSize(c.format, c.width, c.height, 1);
			const u8* src = mMem.read(k.address, size, gpu::MEM_TEXTURE);
			if (src && gpu::hashMemory(src, size) != c.memHash) {
				valid = false;
			}
			checked(c.checkedFrame, c.interval, !valid, k.address);
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
		// (texture memory changes only with palette loads: the hash is kept until the next one)
		TlutHash& th = mTlutCache[map];
		if (th.serial != mTlutSerial || th.tmem != tlut || th.bytes != bytes) {
			th.serial = mTlutSerial;
			th.tmem   = tlut;
			th.bytes  = bytes;
			th.hash   = gpu::hashMemory(mTmem + tlut, bytes);
		}
		k.tlutHash = th.hash;
	}
	TexEntry& e = mTextures[k];
	if (e.tex && !checkDue(e.checkedFrame, e.interval)) {
		if (!mAudit || e.usedFrame == mFrame) {
			e.usedFrame = mFrame;
			return e.tex;
		}
		// (an audit frame: checked anyway, once)
		const u32 size = gpu::CommandProcessor::textureSize(k.format, k.width, k.height, 1);
		const u8* src  = k.address ? mMem.read(k.address, size, gpu::MEM_TEXTURE) : NULL;
		mAuditCounts.textures++;
		if (!src || gpu::hashMemory(src, size) == e.hash) {
			e.usedFrame = mFrame;
			return e.tex;
		}
		if (mAuditCounts.texturesChanged++ < AUDIT_LOGS) {
			log("[audit] frame %u: texture %ux%u (format %u) at 0x%07x changed without being noticed (checked %u frames ago, every %u)\n",
			    mFrame, k.width, k.height, k.format, k.address, mFrame - e.checkedFrame, e.interval);
		}
	}
	gpu::PhaseScope phase(gpu::GFX_CHECKS);
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
		checked(e.checkedFrame, e.interval, false, k.address);
		e.usedFrame = mFrame;
		return e.tex;
	}
	gpu::PhaseScope decode(gpu::GFX_TEXTURES);
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
	if (mFrame < 300 && mStats.textureUploads <= 24) {
		log("[rsx] frame %u: texture %ux%u, format %u, at 0x%07x, decoded (%s)\n", mFrame, k.width, k.height, k.format, k.address,
		    e.tex ? "changed" : "new");
	}
	mStats.textureUploadBytes += t->bytes;
	const bool replaced = e.tex != NULL;
	retire(e.tex);
	e.tex          = t;
	e.hash         = hash;
	e.usedFrame    = mFrame;
	if (replaced) {
		checked(e.checkedFrame, e.interval, true, k.address);
	} else {
		e.checkedFrame = mFrame;
		e.interval     = 1; // (new: checked again soon, its memory may still be filling)
	}
	return t;
}

// Works out the device state, program and textures for the current registers, redoing only what changed.
// False: nothing to draw.
bool Renderer::updateState()
{
	const gpu::State& S = mCp.state();
	const u32* bp       = S.bp;
	const u32* xf       = S.xf;
	if (memcmp(mViewport, xf + 0x101A, sizeof(mViewport)) != 0) {
		memcpy(mViewport, xf + 0x101A, sizeof(mViewport));
		mDirtyBits |= D_STATE;
	}
	if (!mDirtyBits) {
		return mDrawable;
	}
	gpu::PhaseScope phase(gpu::GFX_STATE);
	const u32 dirty = mDirtyBits;
	mDirtyBits      = 0;

	// ---- device state: blending, depth, color mask, scissor, viewport
	if (dirty & D_STATE) {
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
		mStateOk = sx1 > sx0 && sy1 > sy0;
		rs.scissor[0] = (u16)sx0, rs.scissor[1] = (u16)sy0;
		rs.scissor[2] = (u16)(sx1 - sx0), rs.scissor[3] = (u16)(sy1 - sy0);
		const float* xff     = (const float*)xf;
		rs.viewportScale[0]  = xff[0x101A];
		rs.viewportScale[1]  = xff[0x101B];
		rs.viewportScale[2]  = xff[0x101C] * (1.0f / 16777215.0f);
		rs.viewportOffset[0] = xff[0x101D] - 342.0f;
		rs.viewportOffset[1] = xff[0x101E] - 342.0f;
		rs.viewportOffset[2] = xff[0x101F] * (1.0f / 16777215.0f);
		rs.cullFace          = mState.cullFace; // (set per draw: drawGpu, drawCpu)
		if (mStateOk && (!mStateValid || rs != mState)) {
			flushBatch();
			gpu::PhaseScope rsx(gpu::GFX_RSX);
			mDev.setState(rs);
			mState      = rs;
			mStateValid = true;
		}
		mCull = bits(bp[0x00], 14, 2);
	}

	// ---- fragment program for the TEV set-up
	if (dirty & D_TEV) {
		TevKey key;
		makeTevKey(bp, key);
		if (!mProgram || !(mProgram->key == key)) {
			Vec<ProgramEntry*>& chain = mPrograms[key.hash()];
			ProgramEntry* found       = NULL;
			for (size_t i = 0; i < chain.size(); i++) {
				if (chain[i]->key == key) {
					found = chain[i];
					break;
				}
			}
			if (!found) {
				gpu::PhaseScope programs(gpu::GFX_PROGRAMS);
				found      = create<ProgramEntry>();
				found->key = key;
				const char* error;
				found->ok = buildTevProgram(key, found->prog, &error);
				found->texCoordCount = 0;
				for (u32 t = 0; t < 8; t++) {
					if (found->prog.texCoords & (1u << t)) {
						found->texCoordCount = t + 1;
					}
				}
				mStats.programs++;
				if (!found->ok) {
					mStats.programErrors++;
					if (mStats.programErrors <= 3) {
						log("[rsx] FAILED to assemble a TEV program (%s):\n", error ? error : "?");
						const Str& t = found->prog.text;
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
	}
	const bool programOk = mProgram && mProgram->ok && !mProgram->prog.alwaysKill;

	// ---- its parameters: TEV registers, constants, alpha test references (kept as floats by bpWritten)
	if (programOk && (dirty & (D_TEV | D_PARAMS))) {
		const u32 used = mProgram->prog.params;
		for (u32 i = 0; i < NUM_TEV_PARAMS; i++) {
			if ((used & (1u << i)) && memcmp(mRegF[i], mParams[i], sizeof(mParams[i])) != 0) {
				memcpy(mParams[i], mRegF[i], sizeof(mParams[i]));
				mProgramBound = false;
			}
		}
	}
	if (programOk && !mProgramBound) {
		flushBatch();
		bindProgram(mProgram->prog.fp, mParams);
		mProgramBound = true;
	}

	// ---- textures
	if (programOk && (dirty & (D_TEV | D_TEX))) {
		for (u32 m = 0; m < 8; m++) {
			if (!(mProgram->prog.texMaps & (1u << m))) {
				continue;
			}
			Sampler s;
			const Texture* t = lookupTexture(m, s);
			if (!mBound[m].valid || mBound[m].tex != t || !(mBound[m].sampler == s)) {
				flushBatch();
				gpu::PhaseScope rsx(gpu::GFX_RSX);
				mDev.setTexture(m, t, s);
				mBound[m].tex     = t;
				mBound[m].sampler = s;
				mBound[m].valid   = true;
			}
		}
	}
	mDrawable = programOk && mStateOk && mCull != 3;
	return mDrawable;
}

void Renderer::bpWritten(u32 reg, u32 value)
{
	mDirtyBits |= sBpDirty[reg & 0xFF];
	if (reg >= 0xE0 && reg < 0xE8) {
		tevRegFromBp(mTevReg, mKonst, reg, value);
		// (as floats for the program parameters: converted here, once per change)
		const int n   = (int)(reg - 0xE0) >> 1;
		const bool k  = (value >> 23) & 1;
		const int* rv = k ? mKonst[n] : mTevReg[n];
		float* f      = mRegF[k ? 4 + n : n];
		for (int c = 0; c < 4; c++) {
			f[c] = rv[c] * (1.0f / 255.0f);
		}
	} else if (reg == 0xF3) {
		mRegF[8][0] = (float)bits(value, 0, 8);
		mRegF[8][1] = (float)bits(value, 8, 8);
	} else if (reg == 0x65) {
		// TLUT load: from memory (BP 0x64) into texture memory
		mTlutSerial++;
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
	if (!call.count || !call.data) {
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
	gpu::PhaseScope drawPhase(gpu::GFX_STATE);
	if (!updateState()) {
		mStats.skippedDraws++;
		return;
	}
	const gpu::State& S = mCp.state();
	// (the transform set-up changes only with XF loads that reach lights or registers, or a new layout)
	{
		const u64 xfSerial = mCp.stats().xfSetupLoads;
		const bool sameLayout = (call.layout == mXfLayoutAt && mCp.layoutVersion() == mXfLayoutVersion)
		                     || memcmp(&mXfLayout, call.layout, sizeof(mXfLayout)) == 0;
		if (!mXfValid || xfSerial != mXfSerial || !sameLayout) {
			gpu::PhaseScope phase(gpu::GFX_XF_SETUP);
			mXfLayout = *call.layout;
			gpu::xfPrepare(S.xf, mXfLayout, mXf);
			if (xfSerial != mXfSerial) {
				mSetupDirty = true; // (the set-up constants do not depend on the layout)
			}
			mXfSerial      = xfSerial;
			mXfValid       = true;
			mXfLookupValid = false;
			mXfLimitFp     = NULL;
		}
		mXfLayoutAt      = call.layout;
		mXfLayoutVersion = mCp.layoutVersion();
	}
	const u32 texCoords = mProgram->texCoordCount;
	if (mXfLimitFp != mProgram) {
		gpu::xfLimit(mXf, mProgram->prog.colorChans, texCoords);
		mXfLimitFp = mProgram;
	}
	XfProgramEntry* xp = mGpuXf ? xfProgramFor(texCoords) : NULL;
	if (!xp || !drawGpu(call, xp)) {
		drawCpu(call, texCoords);
	}
}

// The vertex program for the current set-up (cached until the set-up, the layout or the fragment program's
// needs change). NULL: transform on the CPU.
Renderer::XfProgramEntry* Renderer::xfProgramFor(u32 texCoords)
{
	if (mXfLookupValid && mXfLookupFp == mProgram && mXfLookupTexCoords == texCoords) {
		return mXfLookup;
	}
	gpu::PhaseScope phase(gpu::GFX_PROGRAMS);
	mXfLookupValid     = true;
	mXfLookupFp        = mProgram;
	mXfLookupTexCoords = texCoords;
	mXfLookup          = NULL;
	XfKey key;
	if (!makeXfKey(mXf, mXfLayout, texCoords, mDev.vertexIndexType(), key)) {
		return NULL;
	}
	Vec<XfProgramEntry*>& chain = mXfPrograms[key.hash()];
	for (size_t i = 0; i < chain.size(); i++) {
		if (chain[i]->key == key) {
			mXfLookup = chain[i]->ok ? chain[i] : NULL;
			return mXfLookup;
		}
	}
	XfProgramEntry* e = create<XfProgramEntry>();
	e->key            = key;
	const char* error = NULL;
	e->ok             = buildXfProgram(key, e->prog, e->info, &error);
	mStats.xfPrograms++;
	if (!e->ok) {
		mStats.xfProgramErrors++;
		if (mStats.xfProgramErrors <= 3) {
			log("[rsx] FAILED to build a vertex program (%s)\n", error ? error : "?");
		}
	}
	chain.push_back(e);
	mXfLookup = e->ok ? e : NULL;
	return mXfLookup;
}

// The device's culling for the draws it transforms: the GameCube's front faces have negative screen areas
// (GX_CULL_FRONT removes those), the device's are what frontFaceSign says.
void Renderer::setDeviceCull(u8 cull)
{
	if (mState.cullFace == cull) {
		return;
	}
	flushBatch();
	mState.cullFace = cull;
	if (mStateValid) {
		gpu::PhaseScope rsx(gpu::GFX_RSX);
		mDev.setState(mState);
	}
}

// ---- draws transformed by the RSX

void Renderer::xfWritten(u32 address, u32 count)
{
	const u32 end = address + count;
	if (address < 0x100) {
		// matrix rows (4 words each): out of date in the copy
		const u32 r0 = address >> 2, r1 = ((end < 0x100 ? end : 0x100) - 1) >> 2;
		const u64 hiBits = r1 >= 63 ? ~0ULL : ((1ULL << (r1 + 1)) - 1);
		mStale.lo |= hiBits & ~((1ULL << r0) - 1);
	}
	if (end > 0x400 && address < 0x460) {
		// normal rows (3 words each): copied at j, j + 32 (and j + 64)
		const u32 a0 = address < 0x400 ? 0x400 : address, a1 = (end < 0x460 ? end : 0x460) - 1;
		for (u32 n = (a0 - 0x400) / 3; n <= (a1 - 0x400) / 3; n++) {
			mNStale.lo |= (1ULL << n) | (1ULL << (n + 32));
			if (n < 2) {
				mNStale.hi |= 1u << n;
			}
		}
	}
	if (end > 0x500) {
		mSetupDirty = true; // (post-transform matrices, lights, registers)
	}
}

// ---- the copy of XF matrix memory in the constants
//
// Row r of XF matrix memory is c[XC_MIRROR + r], the normal matrix rows at c[XC_NMIRROR + j] (row j & 31).
// A draw loads the rows its matrices cover when the copy is older than the last XF load that reached them.
// The pending batch's draws read the copy when the batch is drawn, so before a row they read is loaded
// again, the batch is drawn.

void Renderer::uploadSetup()
{
	if (!mSetupDirty && mSetupValid) {
		return;
	}
	float setup[XC_SETUP_COUNT][4];
	xfSetupConstants(mXf, setup);
	if (!mSetupValid || memcmp(setup, mSetup, sizeof(setup)) != 0) {
		flushBatch();
		gpu::PhaseScope rsx(gpu::GFX_RSX);
		// (only the rows that changed, in runs)
		for (u32 r = 0; r < XC_SETUP_COUNT;) {
			if (mSetupValid && memcmp(setup[r], mSetup[r], sizeof(setup[r])) == 0) {
				r++;
				continue;
			}
			u32 e = r + 1;
			while (e < XC_SETUP_COUNT && (!mSetupValid || memcmp(setup[e], mSetup[e], sizeof(setup[e])) != 0)) {
				e++;
			}
			mDev.setVertexConstants(r, e - r, &setup[r][0]);
			r = e;
		}
		memcpy(mSetup, setup, sizeof(setup));
		mSetupValid = true;
		mStats.setupUploads++;
	}
	mSetupDirty = false;
}

namespace {

// The rows covered by matrices starting at the bits of `starts`, n rows each (2 or 3).
inline void cover(u64 starts, u32 n, u64* lo, u32* hi)
{
	if (n == 3) {
		*lo |= starts | (starts << 1) | (starts << 2);
		*hi |= (u32)((starts >> 62) | (starts >> 63));
	} else {
		*lo |= starts | (starts << 1);
		*hi |= (u32)(starts >> 63);
	}
}

} // namespace

// The rows of the copy a draw's vertices read: position matrices (and their normal matrices), texture
// matrices of 2 and of 3 rows.
void Renderer::matrixRowsUsed(const gpu::DrawCall& call, const XfInfo& info, Rows* rows, Rows* nrows)
{
	u64 pos, texRows[8];
	mCp.matrixRows(call, info.texCount, &pos, texRows);
	Rows r = { 0, 0 }, n = { 0, 0 };
	cover(pos, 3, &r.lo, &r.hi);
	for (u32 t = 0; t < info.texCount; t++) {
		if (info.texMatrices & (1u << t)) {
			cover(texRows[t], (info.texRows3 & (1u << t)) ? 3 : 2, &r.lo, &r.hi);
		}
	}
	if (info.normals) {
		cover(pos, 3, &n.lo, &n.hi);
	}
	*rows  = r;
	*nrows = n;
}

void Renderer::uploadMatrices(const Rows& need, const Rows& nneed)
{
	// the rows whose copy is out of date
	const Rows stale  = { need.lo & mStale.lo, need.hi & mStale.hi };
	const Rows nstale = { nneed.lo & mNStale.lo, nneed.hi & mNStale.hi };
	if (!(stale.lo | stale.hi | nstale.lo | nstale.hi)) {
		return;
	}
	gpu::PhaseScope phase(gpu::GFX_XF_SETUP);
	if ((stale.lo & mBatch.rows.lo) || (stale.hi & mBatch.rows.hi) || (nstale.lo & mBatch.nrows.lo) || (nstale.hi & mBatch.nrows.hi)) {
		flushBatch(); // (its draws read the old values)
	}
	gpu::PhaseScope rsx(gpu::GFX_RSX);
	const float* xf = (const float*)mCp.state().xf;
	// matrix rows, in runs (straight from XF memory: 4 floats a row)
	for (u64 m = stale.lo; m;) {
		const u32 r = (u32)__builtin_ctzll(m);
		u32 e       = r + 1;
		while (e < 64 && ((m >> e) & 1)) {
			e++;
		}
		mDev.setVertexConstants(XC_MIRROR + r, e - r, xf + r * 4);
		mStats.matrixRows += e - r;
		m &= e >= 64 ? 0 : ~((1ULL << e) - 1);
	}
	for (u32 r = 64; r < XC_MIRROR_ROWS; r++) {
		if (stale.hi & (1u << (r - 64))) {
			static const float zero[4] = { 0, 0, 0, 0 };
			mDev.setVertexConstants(XC_MIRROR + r, 1, zero);
			mStats.matrixRows++;
		}
	}
	mStale.lo &= ~stale.lo;
	mStale.hi &= ~stale.hi;
	// normal rows (3 floats each in XF memory)
	for (u32 j = 0; j < XC_MIRROR_ROWS;) {
		const bool st = j < 64 ? ((nstale.lo >> j) & 1) != 0 : ((nstale.hi >> (j - 64)) & 1) != 0;
		if (!st) {
			if (j < 64 && !(nstale.lo >> j)) {
				j = 64; // (none left below 64)
			} else {
				j++;
			}
			continue;
		}
		u32 e = j + 1;
		while (e < XC_MIRROR_ROWS && e - j < 16
		       && (e < 64 ? ((nstale.lo >> e) & 1) != 0 : ((nstale.hi >> (e - 64)) & 1) != 0)) {
			e++;
		}
		float data[16][4];
		for (u32 k = j; k < e; k++) {
			const float* n = xf + 0x400 + (k & 31) * 3;
			float* d       = data[k - j];
			d[0] = n[0], d[1] = n[1], d[2] = n[2], d[3] = 0.0f;
		}
		mDev.setVertexConstants(XC_NMIRROR + j, e - j, &data[0][0]);
		mStats.matrixRows += e - j;
		j = e;
	}
	mNStale.lo &= ~nstale.lo;
	mNStale.hi &= ~nstale.hi;
}

bool Renderer::drawGpu(const gpu::DrawCall& call, XfProgramEntry* xp)
{
	const XfInfo& info = xp->info;
	mStats.gpuDraws++;
	const int sign = mFrontFaceSign;
	u8 cull        = CULL_NONE;
	if (mCull == 1) {
		cull = sign > 0 ? CULL_BACK : CULL_FRONT; // (remove negative areas)
	} else if (mCull == 2) {
		cull = sign > 0 ? CULL_FRONT : CULL_BACK;
	}
	setDeviceCull(cull);
	if (mSetupDirty || !mSetupValid) {
		gpu::PhaseScope phase(gpu::GFX_XF_SETUP);
		uploadSetup();
	}

	// a display list draw: from the vertex cache (time counted as state: a phase switch for each part of
	// every draw costs more than the parts)
	if (mCacheOn && call.inDisplayList) {
		CacheEntry* reuse = NULL;
		CacheEntry* e     = cacheLookup(call, xp, &reuse);
		if (!e) {
			gpu::PhaseScope phase(gpu::GFX_TRIANGLES);
			e = cacheFill(call, xp, reuse);
		}
		if (e && mAudit) {
			auditEntry(e, call, xp);
		}
		if (e) {
			cacheFollow(e);
			const Rows rows = e->rows(), nrows = e->nrows();
			uploadMatrices(rows, nrows);
			batchCached(e, xp);
			mBatch.rows.lo |= rows.lo, mBatch.rows.hi |= rows.hi;
			mBatch.nrows.lo |= nrows.lo, mBatch.nrows.hi |= nrows.hi;
			mStats.triangles += e->indexCount / 3;
			mStats.vertices += call.count;
			return true;
		}
	}

	// unpacked into the ring
	gpu::DrawCall c = call;
	if (!c.arrays) {
		c.arrays = mCp.resolveArrays(call);
	}
	Rows rows, nrows;
	{
		gpu::PhaseScope phase(gpu::GFX_XF_SETUP);
		matrixRowsUsed(c, info, &rows, &nrows);
		uploadMatrices(rows, nrows);
	}
	u32 first;
	u8* out = batchVertices(call.count, info.format.stride, 0, xp, &first);
	if (!out) {
		mStats.skippedDraws++;
		return true;
	}
	mBatch.rows.lo |= rows.lo, mBatch.rows.hi |= rows.hi;
	mBatch.nrows.lo |= nrows.lo, mBatch.nrows.hi |= nrows.hi;
	gpu::GatherSpec G = info.gather;
	G.posMap          = mIdentity;
	G.texMap          = mIdentity;
	mCp.gatherVertices(c, G, out);
	mStats.vertices += call.count;

	gpu::PhaseScope phase(gpu::GFX_TRIANGLES);
	const u32 tris           = triangleCount(call.primitive, call.count);
	Vec<unsigned short>& ixv = mBatch.indices;
	const size_t at          = ixv.size();
	ixv.resize(at + tris * 3);
	if (tris) {
		triangleIndices<unsigned short>(call.primitive, call.count, first, &ixv[at]);
	}
	mStats.triangles += tris;
	mBatch.vertexCount += call.count;
	return true;
}

// ---- the vertex cache

void* Renderer::poolAlloc(u32 bytes, u32 align)
{
	bytes = (bytes + 15) & ~15u;
	if (bytes > POOL_CHUNK) {
		return NULL;
	}
	mPoolUsed       = (mPoolUsed + align - 1) & ~(align - 1); // (the chunks are 128-byte aligned)
	const u32 chunk = mPoolUsed / POOL_CHUNK, at = mPoolUsed % POOL_CHUNK;
	if (at + bytes > POOL_CHUNK || chunk >= mPool.size()) {
		// (the next chunk)
		const u32 next = at == 0 && chunk >= mPool.size() ? chunk : chunk + 1;
		if (next >= mPool.size()) {
			if ((next + 1) * (u32)POOL_CHUNK > (u32)POOL_MAX) {
				return NULL;
			}
			u8* m = (u8*)memalign(128, POOL_CHUNK);
			if (!m) {
				return NULL;
			}
			mPool.push_back(m);
		}
		mPoolUsed = next * POOL_CHUNK;
	}
	u8* p = mPool[mPoolUsed / POOL_CHUNK] + mPoolUsed % POOL_CHUNK;
	mPoolUsed += bytes;
	return p;
}

// Starts the vertex cache over (its memory or its entries ran out): once the RSX has drawn everything that
// reads the cache memory.
void Renderer::cacheReset()
{
	flushBatch();
	// (a fence of its own: the draws from the cache allocate nothing in the ring, so the last marker may be
	// older than them)
	const u32 fence = ++mFenceCounter;
	mDev.writeFence(fence);
	mDev.kick();
	{
		gpu::PhaseScope wait(gpu::GFX_WAIT_RSX);
		while ((int)(mDev.fenceReached() - fence) < 0) {
			mDev.pause();
		}
	}
	release();
	memset(mTable, 0, mTableSlots * sizeof(CacheEntry*));
	mTableUsed = 0;
	mPoolUsed  = 0;
	mArenaHead = 0;
	mIdxHead   = 0;
	mArenaGen++;
	mRegions   = NULL;
	mLastEntry = NULL;
	mStats.cacheResets++;
}

namespace {

// Whether n bytes at a and b are the same, for a and b equally far into an 8-byte word: compared a word at a
// time, the bytes before and after the range masked off (they are in the same words: readable). (The
// library's memcmp compares unaligned bytes one at a time.)
typedef u64 __attribute__((may_alias)) AliasU64;
inline bool sameBytes(const u8* a, const u8* b, u32 n)
{
	if (!n) {
		return true;
	}
	const u32 head     = (u32)((uintptr_t)a & 7);
	const AliasU64* wa = (const AliasU64*)(a - head);
	const AliasU64* wb = (const AliasU64*)(b - head);
	const u32 total    = n + head;
	const u32 words    = (total + 7) >> 3;
	const u32 tail     = total & 7;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	const u64 first = ~0ULL >> (head * 8);
	const u64 last  = tail ? ~(~0ULL >> (tail * 8)) : ~0ULL;
#else
	const u64 first = ~0ULL << (head * 8);
	const u64 last  = tail ? ~(~0ULL << (tail * 8)) : ~0ULL;
#endif
	if (words == 1) {
		return ((wa[0] ^ wb[0]) & first & last) == 0;
	}
	u64 d = (wa[0] ^ wb[0]) & first;
	for (u32 i = 1; i + 1 < words; i++) {
		d |= wa[i] ^ wb[i];
	}
	d |= (wa[words - 1] ^ wb[words - 1]) & last;
	return d == 0;
}

inline u32 tableSlot(const u8* data, u32 slots)
{
	const u64 a = (u64)(uintptr_t)data;
	return (u32)(((a >> 2) ^ (a >> 17)) * 2654435761u) & (slots - 1);
}

} // namespace

// The vertex cache's entry for a display list draw, if it is still what the draw would unpack. NULL: not
// there, or out of date (then *reuse: the entry to fill again).
Renderer::CacheEntry* Renderer::cacheLookup(const gpu::DrawCall& call, XfProgramEntry* xp, CacheEntry** reuse)
{
	// (the draws come in the same order frame after frame: the entry that came after the last one, last
	// time, is usually this draw's, and already read ahead)
	CacheEntry* e = mLastEntry ? mLastEntry->follow : NULL;
	if (e && e->data == call.data && e->xp == xp && e->count == call.count && e->prim == call.primitive
	    && e->size == call.layout->size) {
		mStats.cacheFollowed++;
		mChainLength = 1;
	} else {
		u32 i = tableSlot(call.data, mTableSlots);
		CacheEntry* head = NULL;
		while (mTable[i]) {
			if (mTable[i]->data == call.data) {
				head = mTable[i];
				break;
			}
			i = (i + 1) & (mTableSlots - 1);
		}
		mChainLength = 0;
		for (e = head; e; e = e->next) {
			mChainLength++;
			if (e->xp == xp && e->count == call.count && e->prim == call.primitive && e->size == call.layout->size) {
				break;
			}
		}
		if (!e) {
			return NULL;
		}
	}
	*reuse = e;
	if (!e->cached) {
		return NULL;
	}
	return cacheValid(e, call) ? (mStats.cacheHits++, e) : NULL;
}

// After a draw from the cache: e is the entry that came after the last one, and the one it was followed by
// last time is read ahead for the next draw.
void Renderer::cacheFollow(CacheEntry* e)
{
	if (mLastEntry && mLastEntry->follow != e) {
		mLastEntry->follow      = e;
		mLastEntry->followLines = e->lines;
	}
	mLastEntry = e;
	if (const CacheEntry* f = e->follow) {
		const u8* p = (const u8*)f;
		for (u32 l = 0; l < e->followLines; l++) {
			__builtin_prefetch(p + l * 128);
		}
	}
}

bool Renderer::cacheValid(CacheEntry* e, const gpu::DrawCall& call)
{
	if (e->gen != mArenaGen) {
		return false;
	}
	const u32* cp = mCp.state().cp;
	const u32 vat = call.vat;
	if (e->cp[0] != cp[0x50] || e->cp[1] != cp[0x60] || e->cp[2] != cp[0x70 + vat] || e->cp[3] != cp[0x80 + vat]
	    || e->cp[4] != cp[0x90 + vat] || e->cp[5] != cp[0x30] || e->cp[6] != cp[0x40]) {
		return false;
	}
	if (!sameBytes(call.data, e->bytes(), e->count * e->size)) {
		return false;
	}
	// the arrays: where they are, and whether they changed (unless they were found unchanged earlier in this
	// frame: their versions change only when they are checked, at most once a frame)
	const CacheArray* arr = e->arr();
	const bool recheck    = e->okFrame != mFrame;
	for (u32 k = 0, n = e->arrays; k < n; k++) {
		const CacheArray& a = arr[k];
		if (cp[0xA0 + a.array] != a.base || (cp[0xB0 + a.array] & 0xFF) != a.stride) {
			return false;
		}
		if (recheck) {
			ArrayRegion& r = *a.region;
			if (checkDue(r.checkedFrame, r.interval)) {
				checkRegion(r);
			} else if (mAudit && r.hashValid && r.checkedFrame != mFrame && r.auditFrame != mFrame) {
				// (an audit frame: checked anyway, once)
				r.auditFrame    = mFrame;
				const u32 bytes = (r.hi - r.lo) * r.stride + r.stride;
				const u8* p     = mMem.read(r.base + r.lo * r.stride, bytes, gpu::MEM_ARRAY);
				mAuditCounts.regions++;
				if (p && gpu::hashMemory(p, bytes) != r.hash) {
					if (mAuditCounts.regionsChanged++ < AUDIT_LOGS) {
						log("[audit] frame %u: vertex array %u at 0x%07x (entries %u-%u) changed without being noticed (checked %u frames "
						    "ago, every %u)\n",
						    mFrame, r.array, r.base, r.lo, r.hi, mFrame - r.checkedFrame, r.interval);
					}
					r.checkedFrame = mFrame - r.interval; // (due: checked, a new version)
					checkRegion(r);
				}
			}
			if (r.version != a.version) {
				return false;
			}
		}
	}
	e->okFrame = mFrame;
	return true;
}

// Hashes a vertex array's range (when due): a new version if it changed.
void Renderer::checkRegion(ArrayRegion& r)
{
	gpu::PhaseScope phase(gpu::GFX_CHECKS);
	mStats.regionChecks++;
	const u32 bytes = (r.hi - r.lo) * r.stride + r.stride;
	const u8* p     = mMem.read(r.base + r.lo * r.stride, bytes, gpu::MEM_ARRAY);
	const u64 h     = p ? gpu::hashMemory(p, bytes) : 0;
	bool changed    = false;
	if (!r.hashValid) {
		r.hashValid = true; // (a new range: nothing to compare with)
	} else if (h != r.hash) {
		changed = true;
		r.version++;
		mStats.regionChanges++;
	}
	r.hash = h;
	checked(r.checkedFrame, r.interval, changed, r.base);
}

// (NULL: no memory for it)
Renderer::ArrayRegion* Renderer::regionFor(u32 array, u32 base, u32 stride)
{
	for (ArrayRegion* r = mRegions; r; r = r->next) {
		if (r->base == base && r->stride == (stride ? stride : 1) && r->array == array) {
			return r;
		}
	}
	ArrayRegion* r = (ArrayRegion*)poolAlloc(sizeof(ArrayRegion), 16);
	if (!r) {
		return NULL;
	}
	r->next         = mRegions;
	r->array        = array;
	r->base         = base;
	r->stride       = stride ? stride : 1;
	r->lo           = 0xFFFFFFFF;
	r->hi           = 0;
	r->hash         = 0;
	r->hashValid    = false;
	r->version      = 1;
	r->checkedFrame = mFrame;
	r->auditFrame   = 0;
	r->interval     = 1;
	mRegions        = r;
	return r;
}

// Unpacks a display list draw into the vertex cache (into e: an out of date entry for it, or NULL for a new
// one). NULL: not cached (it changes too often, or does not fit); the draw goes through the ring instead.
Renderer::CacheEntry* Renderer::cacheFill(const gpu::DrawCall& call, XfProgramEntry* xp, CacheEntry* e)
{
	if (e && !e->cached) {
		mStats.cacheUncached++;
		return NULL;
	}
	if (e && ++e->refills >= 4) {
		e->cached = false; // (what it reads changes too often: from the ring from now on)
		mStats.cacheUncached++;
		return NULL;
	}
	if (!e && mChainLength >= 4) {
		// (a display list address drawn in many different ways: probably made anew each frame)
		mStats.cacheUncached++;
		return NULL;
	}
	const gpu::VertexLayout& L = *call.layout;
	const u32 stride           = xp->info.format.stride;
	const u32 indexCount       = triangleCount(call.primitive, call.count) * 3;
	const u32 dataBytes        = call.count * L.size;
	const u32 vertexBytes      = call.count * stride;
	const u32 arrayCap         = L.indexedCount;
	const u32 bytesOff         = (u32)((uintptr_t)call.data & 7);
	const u32 entryBytes       = sizeof(CacheEntry) + ((bytesOff + dataBytes + 15) & ~15u) + arrayCap * sizeof(CacheArray);
	if (!stride || vertexBytes > mArenaSize / 4 || indexCount > mIdxSize / 4 || entryBytes > POOL_CHUNK / 4 || L.size > 255
	    || arrayCap > CACHE_ARRAYS || (e && arrayCap > e->arrayCap)) {
		mStats.cacheUncached++;
		return NULL;
	}
	// room: in the cache memory (vertices at a multiple of the stride from its start, for the indices), the
	// index memory, the table and the entries' memory
	u32 off = (mArenaHead + stride - 1) / stride * stride;
	if (off + vertexBytes > mArenaSize || mIdxHead + indexCount > mIdxSize || mTableUsed >= (u32)CACHE_ENTRIES
	    || mTableUsed * 2 >= mTableSlots || mPoolUsed + entryBytes + 256 > (u32)(POOL_MAX - 2 * POOL_CHUNK)) {
		cacheReset();
		e   = NULL;
		off = 0;
	}
	const bool fresh = e == NULL;
	if (fresh) {
		// (an entry starts a 128-byte cache line: what every draw from it reads first is one line)
		e = (CacheEntry*)poolAlloc(entryBytes, 128);
		if (!e) {
			mStats.cacheUncached++;
			return NULL;
		}
		memset(e, 0, sizeof(*e));
		e->data     = call.data;
		e->xp       = xp;
		e->count    = (u16)call.count;
		e->prim     = (u8)call.primitive;
		e->size     = (u8)L.size;
		e->arrayCap = (u8)arrayCap;
		e->bytesOff = (u8)bytesOff;
		e->lines    = (u8)((entryBytes + 127) / 128 < 8 ? (entryBytes + 127) / 128 : 8);
		e->cached   = true;
	}
	// (a refill keeps its place in the entries' memory: the bytes and arrays are the CPU's, the vertices
	// the RSX reads are unpacked to new cache memory)
	const gpu::VertexArray* arrays = call.arrays ? call.arrays : mCp.resolveArrays(call);
	gpu::DrawCall c = call;
	c.arrays        = arrays;
	memcpy(e->bytes(), call.data, dataBytes);
	const u32* cp = mCp.state().cp;
	e->cp[0] = cp[0x50], e->cp[1] = cp[0x60], e->cp[2] = cp[0x70 + call.vat], e->cp[3] = cp[0x80 + call.vat];
	e->cp[4] = cp[0x90 + call.vat], e->cp[5] = cp[0x30], e->cp[6] = cp[0x40];
	// the arrays it reads, and the ranges
	CacheArray* arr = e->arr();
	e->arrays       = 0;
	for (u32 k = 0; k < L.indexedCount; k++) {
		const u32 id              = L.indexed[k].array;
		const gpu::VertexArray& A = arrays[id];
		if (!A.used) {
			continue;
		}
		bool dup = false;
		for (u32 j = 0; j < e->arrays; j++) {
			dup |= arr[j].array == id;
		}
		if (dup) {
			continue;
		}
		CacheArray& ca = arr[e->arrays++];
		ca.array       = (u8)id;
		ca.base        = cp[0xA0 + id];
		ca.stride      = (u8)(cp[0xB0 + id] & 0xFF);
		ArrayRegion* r = regionFor(id, ca.base, ca.stride);
		if (!r) {
			e->cached = false; // (no memory: from the ring)
			mStats.cacheUncached++;
			return NULL;
		}
		if (A.minIndex < r->lo || A.maxIndex > r->hi) {
			r->lo           = A.minIndex < r->lo ? A.minIndex : r->lo;
			r->hi           = A.maxIndex > r->hi ? A.maxIndex : r->hi;
			r->hashValid    = false; // (hashed at its next check: soon)
			r->interval     = 1;
			r->checkedFrame = mFrame;
		}
		ca.region  = r;
		ca.version = r->version;
	}
	// the vertices, unpacked into the cache memory, and the triangles
	{
		gpu::GatherSpec G = xp->info.gather;
		G.posMap          = mIdentity;
		G.texMap          = mIdentity;
		mCp.gatherVertices(c, G, mArena + off);
	}
	e->firstVertex = off / stride;
	e->indexAt     = mIdxHead;
	e->indexCount  = triangleIndices<u32>(call.primitive, call.count, e->firstVertex, mIdx + mIdxHead);
	mIdxHead += e->indexCount;
	Rows rows, nrows;
	matrixRowsUsed(c, xp->info, &rows, &nrows);
	e->rowsLo = rows.lo, e->rowsHi = rows.hi;
	e->nrowsLo = nrows.lo, e->nrowsHi = nrows.hi;
	mArenaHead = off + vertexBytes;
	e->gen     = mArenaGen;
	e->okFrame = mFrame;
	if (fresh) {
		// into the table (after the entries for the same data, if any)
		u32 i = tableSlot(call.data, mTableSlots);
		while (mTable[i] && mTable[i]->data != call.data) {
			i = (i + 1) & (mTableSlots - 1);
		}
		if (mTable[i]) {
			e->next         = mTable[i]->next;
			mTable[i]->next = e;
		} else {
			mTable[i] = e;
			mTableUsed++;
		}
	}
	mStats.cacheFills++;
	return e;
}

// An audit frame: the entry's vertices, triangles and matrix rows compared with what unpacking the draw
// again gives.
void Renderer::auditEntry(CacheEntry* e, const gpu::DrawCall& call, XfProgramEntry* xp)
{
	gpu::PhaseScope phase(gpu::GFX_CHECKS);
	mAuditCounts.draws++;
	const u32 stride = xp->info.format.stride;
	const u32 bytes  = call.count * stride;
	gpu::DrawCall c  = call;
	c.arrays         = call.arrays ? call.arrays : mCp.resolveArrays(call);
	mAuditVerts.resize(bytes + 64);
	{
		gpu::GatherSpec G = xp->info.gather;
		G.posMap          = mIdentity;
		G.texMap          = mIdentity;
		mCp.gatherVertices(c, G, &mAuditVerts[0]);
	}
	// (the bytes of a vertex the unpacking writes: the matrix index groups have room for 4 indices, the
	// unused ones are left as they are)
	u8 written[256];
	memset(written, 0, sizeof(written));
	const VertexFormat& F = xp->info.format;
	for (u32 i = 0; i < 16; i++) {
		const VertexAttrib& a = F.attr[i];
		if (!a.components || i == XA_IDX_A || i == XA_IDX_B || i == XA_IDX_C) {
			continue;
		}
		const u32 n = a.components * (a.type == ATTRIB_F32 ? 4u : 1u);
		for (u32 b = a.offset; b < a.offset + n && b < sizeof(written); b++) {
			written[b] = 1;
		}
	}
	const gpu::GatherSpec& G0 = xp->info.gather;
	for (u32 t = 0; t < G0.indexCount; t++) {
		const u32 o = G0.index + (G0.indexFloat ? t * 4 : t), n = G0.indexFloat ? 4 : 1;
		for (u32 b = o; b < o + n && b < sizeof(written); b++) {
			written[b] = 1;
		}
	}
	const u8* kept   = mArena + e->firstVertex * stride;
	const char* what = NULL;
	u32 at           = 0;
	for (; at < bytes && stride <= sizeof(written); at++) {
		if (written[at % stride] && mAuditVerts[at] != kept[at]) {
			what = "vertices";
			break;
		}
	}
	if (!what) {
		mAuditIdx.resize(e->indexCount + 8);
		const u32 n = triangleIndices<u32>(call.primitive, call.count, e->firstVertex, &mAuditIdx[0]);
		if (n != e->indexCount || (n && memcmp(&mAuditIdx[0], mIdx + e->indexAt, n * 4) != 0)) {
			what = "triangles";
		}
	}
	if (!what) {
		Rows rows, nrows;
		matrixRowsUsed(c, xp->info, &rows, &nrows);
		if (rows.lo != e->rowsLo || rows.hi != e->rowsHi || nrows.lo != e->nrowsLo || nrows.hi != e->nrowsHi) {
			what = "matrix rows";
		}
	}
	if (what && mAuditCounts.drawsWrong++ < AUDIT_LOGS) {
		log("[audit] frame %u: a draw from the vertex cache has the wrong %s (%u vertices of %u bytes, %u in the cache, primitive 0x%02x, "
		    "list bytes at %p; first difference: vertex %u, byte %u; filled %u times)\n",
		    mFrame, what, call.count, call.layout->size, stride, call.primitive, (const void*)call.data, at / stride, at % stride,
		    e->refills + 1u);
	}
}

// ---- draws transformed on the CPU (gpu_xf.cpp), culled on the CPU

void Renderer::drawCpu(const gpu::DrawCall& call, u32 texCoords)
{
	mStats.cpuDraws++;
	setDeviceCull(CULL_NONE);
	const gpu::Vertex* vertices = call.vertices;
	if (!vertices) {
		gpu::PhaseScope phase(gpu::GFX_DECODE);
		vertices = mCp.decodeDraw(call);
		if (!vertices) {
			mStats.skippedDraws++;
			return;
		}
	}
	if (mXf.texLimit > texCoords) {
		texCoords = mXf.texLimit; // (an emboss texgen: the coordinates before it are worked out too)
	}
	u32 first;
	u8* out = batchVertices(call.count, VERTEX_BASE_BYTES + VERTEX_TEX_BYTES * texCoords, texCoords, NULL, &first);
	if (!out) {
		mStats.skippedDraws++;
		return;
	}
	const u32 stride = mBatch.stride;

	// Vertices go straight into the draw memory: the layout is XfVertex's, cut after texCoords coordinates.
	{
		gpu::PhaseScope phase(gpu::GFX_TRANSFORM);
		const gpu::XfSetup& xs = mXf;
		for (u32 i = 0; i < call.count; i++) {
			gpu::xfTransform(xs, vertices[i], *(gpu::XfVertex*)(out + i * stride));
		}
	}
	mStats.vertices += call.count;
	gpu::PhaseScope phase(gpu::GFX_TRIANGLES);
	const u32 prim = call.primitive;

	// Triangles, without those facing away (sign of the screen-space area, from the homogeneous positions:
	// correct also for triangles that cross the camera plane).
	const float xyScale = mState.viewportScale[0] * mState.viewportScale[1];
	const u32 cull      = mCull;
	Vec<unsigned short>& ix = mBatch.indices;
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
	u8* out = batchVertices(4, VERTEX_BASE_BYTES + VERTEX_TEX_BYTES, 1, NULL, &first);
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
		for (int k = 4; k < 12; k++) {
			f[k] = 1.0f; // (colors)
		}
		f[12] = u[i], f[13] = v[i], f[14] = 1.0f;
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
		Str text = copyProgramText(COPY_RGB, false);
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
	// the test switches, beside the picture: as many red squares as the test mode's number, and a blue one
	// below them while everything is checked every frame
	if (x0 >= 48 && (mMode != 0 || mCheckEveryFrame)) {
		const u16 sq = (u16)(x0 / 4 < 32 ? x0 / 4 : 32);
		const u16 sx = (u16)(x0 / 2 - sq / 2);
		u16 y        = (u16)(y0 + 40);
		for (u32 i = 0; i < mMode; i++, y = (u16)(y + sq * 2)) {
			const u16 r[4] = { sx, y, sq, sq };
			mDev.clear(r, 0xFFE02020, 0, true, true, false);
		}
		if (mCheckEveryFrame) {
			const u16 r[4] = { sx, (u16)(y + sq), sq, sq };
			mDev.clear(r, 0xFF2040FF, 0, true, true, false);
		}
	}
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
		Str text = copyProgramText(mode, in != 0);
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
	c.interval     = 1;
	u32 size       = gpu::CommandProcessor::textureSize(format, w, h, 1);
	const u8* mem  = mMem.read(address, size, gpu::MEM_TEXTURE);
	c.memKnown     = mem != NULL;
	c.memHash      = mem ? gpu::hashMemory(mem, size) : 0;
	// (decoded textures at that address are out of date)
	for (Map<TexKey, TexEntry>::iterator it = mTextures.begin(); it != mTextures.end();) {
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
	gpu::PhaseScope phase(gpu::GFX_COPIES);
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
	// (has the RSX finished the previous frame by the time this one is submitted? If not, it is the
	// slower of the two)
	if (mFrameFence && (int)(mDev.fenceReached() - mFrameFence) < 0) {
		mStats.rsxBehind++;
	}
	mFrame++;
	if (mAudit) {
		const Audit& a = mAuditCounts;
		log("[audit] frame %u: %u draws from the vertex cache compared (%u wrong); %u textures and %u vertex arrays checked early "
		    "(%u and %u had changed)\n",
		    mFrame - 1, a.draws, a.drawsWrong, a.textures, a.regions, a.texturesChanged, a.regionsChanged);
		memset(&mAuditCounts, 0, sizeof(mAuditCounts));
	}
	mAudit = mFrame % AUDIT_FRAMES == 0;
	if (mResetRequest) {
		mResetRequest = 0;
		if (mTableUsed) {
			cacheReset();
		}
	}
	if (mSwitchRequest) {
		const int sw = __sync_fetch_and_and(&mSwitchRequest, 0);
		if (sw & SWITCH_MODE) {
			// the next test mode (the vertex cache started over: its vertex arrays are not checked while it is
			// off)
			static const char* const kNames[MODE_COUNT] = {
				"normal (the texture cache dropped before each new fragment program)",
				"as test build 30 (fragment programs used as written)",
				"as test build 30, each fragment program in a 4 KB block of its own",
				"normal, the graphics thread paused 8 ms after each frame",
				"normal, vertex cache triangles in the command buffer",
				"normal, vertex cache OFF",
			};
			mMode = (mMode + 1) % MODE_COUNT;
			if (mTableUsed) {
				cacheReset();
			}
			setVertexCache(mMode != MODE_NOCACHE);
			mDev.setProgramCacheDrop(mMode != MODE_OLD && mMode != MODE_SPACED);
			log("[rsx] frame %u: test switch (R3): mode %u, %s%s\n", mFrame, mMode, kNames[mMode],
			    mCacheOn == (mMode != MODE_NOCACHE) ? "" : " (not possible here)");
		}
		if (sw & SWITCH_CHECKS) {
			mCheckEveryFrame = !mCheckEveryFrame;
			log("[rsx] frame %u: test switch (L3): textures and vertex arrays checked %s\n", mFrame,
			    mCheckEveryFrame ? "EVERY FRAME" : "as usual");
		}
	}
	if (mFrame <= 300 && mFrame % 10 == 0) {
		// (the first frames, the loading screens' among them: what was drawn)
		static RendererStats early;
		log("[rsx] frames %u-%u: %llu draws (%llu skipped, %llu on the CPU), %llu triangles, %llu batches, %llu textures uploaded (%llu KB), "
		    "%llu clears\n",
		    mFrame - 9, mFrame, mStats.draws - early.draws, mStats.skippedDraws - early.skippedDraws, mStats.cpuDraws - early.cpuDraws,
		    mStats.triangles - early.triangles, mStats.batches - early.batches, mStats.textureUploads - early.textureUploads,
		    (mStats.textureUploadBytes - early.textureUploadBytes) >> 10, mStats.clears - early.clears);
		early = mStats;
	}
	mDirtyBits = D_ALL; // (textures are checked again once per frame)
	if (mMode == MODE_SLOW) {
		gpu::PhaseScope wait(gpu::GFX_WAIT_RSX);
		mDev.sleepUs(8000);
	}
	mark();
	mFrameFence = mFenceCounter;
	mDev.kick();
	release();
	evictTextures();
	mStats.textureBytes = mTextureBytes;
	mStats.textures     = (u32)mTextures.size();
	mStats.cacheBytes   = mArenaHead;
	mStats.cacheEntries = mTableUsed;
}

// Textures not used in the last two frames go when the decoded textures take more than the budget.
void Renderer::evictTextures()
{
	if (mTextureBytes <= TEXTURE_BUDGET) {
		return;
	}
	for (Map<TexKey, TexEntry>::iterator it = mTextures.begin(); it != mTextures.end() && mTextureBytes > TEXTURE_BUDGET * 3 / 4;) {
		if (it->second.usedFrame + 2 <= mFrame) {
			retire(it->second.tex);
			mTextures.erase(it++);
		} else {
			++it;
		}
	}
}

} // namespace gfx
