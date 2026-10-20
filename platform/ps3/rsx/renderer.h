// PS3 port: draws the GameCube's graphics commands with a modern-ish GPU (the PS3's RSX, through the
// Device interface in device.h).
//
// The command processor (platform/ps3/gpu/gpu_cp.h) reports each draw. The renderer
//   - has the RSX transform and light the vertices: the XF set-up becomes a vertex program
//     (xf_program.h), XF matrix memory is copied into vertex program constants (the rows the draws use,
//     when they change), and the CPU only unpacks the vertices into the RSX's memory
//     (CommandProcessor::gatherVertices). Set-ups the vertex programs do not cover (emboss texture
//     coordinates), or a device whose vertex programs failed the start-up self-test, are transformed on the
//     CPU instead (gpu_xf.h, the same code as the host's reference renderer), with back faces removed the
//     way the GameCube does;
//   - keeps the unpacked vertices of display list draws (the vertex cache): the game draws the same
//     display lists frame after frame, so a draw whose display list bytes, vertex format and vertex arrays
//     are unchanged is drawn from what was unpacked before (the arrays' memory is re-checked from time to
//     time, the way textures are);
//   - turns the pixel pipeline set-up (TEV, alpha test) into a fragment program (tev_program.h), the
//     blending, depth and scissor settings into device state, and the textures the stages sample into
//     device textures (decoded on the CPU, cached, re-checked once per frame);
//   - collects consecutive draws that share all of that into one batch of indexed triangles: the game
//     makes up to ~12,000 small draws per frame;
//   - implements copies out of the embedded frame buffer: to textures (rendered into a device texture
//     that later draws sample in place of the copy's memory), and to the display (the frame is shown).
//
// Portable C++: the host tools run it with a software device to check it against the reference renderer.

#ifndef PS3_RSX_RENDERER_H
#define PS3_RSX_RENDERER_H

#include "../gpu/gpu_cp.h"
#include "../gpu/gpu_xf.h"
#include "device.h"
#include "tev_program.h"
#include "xf_program.h"

#include "rsx_alloc.h"

namespace gfx {

typedef unsigned long long u64;

struct RendererStats {
	u64 draws, batches, triangles, culled, vertices, skippedDraws, lineDraws, pointDraws;
	u64 programs, programErrors, programBinds, textureUploads, textureUploadBytes, missingTextures;
	u64 copies, copyUses, presents, clears, fenceWaits;
	u64 gpuDraws, cpuDraws, xfPrograms, xfProgramErrors, matrixRows, setupUploads, vertexProgramLoads;
	u64 cacheHits, cacheFills, cacheUncached, cacheResets, regionChecks, regionChanges;
	u64 cacheFollowed; // (draws whose entry was the one predicted)
	u64 refillWhy[5];  // why entries were out of date: started over, CP registers, list bytes, array places, array contents
	u64 cacheVariants; // (entries made for a draw already in the cache, drawn with other registers or array places)
	u32 cacheBytes, cacheEntries;
	u64 rsxBehind; // frames finished before the RSX had drawn the previous one
	u32 textureBytes, textures;
};

class Renderer : public gpu::Listener {
public:
	Renderer(gpu::CommandProcessor& cp, gpu::Memory& memory, Device& device);
	~Renderer();

	// Takes the TEV register values from the command processor's state (call before the first command).
	void begin();
	// Submits what is pending (end of a trace on the host).
	void flush();
	// Reads a rectangle of the embedded frame buffer (0xAARRGGBB) once the chip has drawn everything so far.
	bool readEfb(u32* out, u32 x, u32 y, u32 width, u32 height);

	void setLogger(void (*log)(const char* text)) { mLog = log; }

	// (with the transform on the RSX, vertices are unpacked straight into its memory, the vertex arrays
	// worked out only for draws that are not in the vertex cache: nothing asked of the command processor)
	gpu::VertexDetail vertexDetail() override { return mGpuXf ? gpu::VERTICES_NONE : gpu::VERTICES_DECODED; }
	// (a run of display list draws is one vertex cache entry: test build 34 spent about 2 microseconds on each
	// draw, and the Forest of Hope's ground has some 25000 of 1 triangle each)
	bool drawRuns() override { return mGpuXf && mMode != MODE_NORUNS; }
	void draw(const gpu::DrawCall& call) override;
	void xfWritten(u32 address, u32 count) override;
	// Transform on the RSX when it can (the default when the device's vertex programs work), or always on
	// the CPU.
	void setGpuTransform(bool on);
	bool gpuTransform() const { return mGpuXf; }
	// The vertex cache (on by default with the transform on the RSX, if the device draws 32-bit indices).
	void setVertexCache(bool on);
	bool vertexCache() const { return mCacheOn; }
	// Starts the vertex cache over at the end of the frame (from any thread).
	void requestCacheReset() { mResetRequest = 1; }
	// Test switches, applied at the end of the frame (from any thread). MODE: the next test mode (MODE_*,
	// in turn). CHECKS: every texture and vertex array checked for changes every frame, or as usual (in
	// turn). Shown by squares beside the picture (as many as the mode's number; blue: the checks).
	enum { SWITCH_MODE = 1, SWITCH_CHECKS = 2 };
	enum {
		MODE_NORMAL,  // the texture cache dropped before each new fragment program is used (Device::setProgram)
		MODE_OLD,     // as test build 30: fragment programs used as they are written
		MODE_SPACED,  // as test build 30, each fragment program in a 4 KB block of its own
		MODE_SLOW,    // as normal, the graphics thread paused 8 ms after each frame
		MODE_INLINE,  // as normal, the vertex cache's triangles copied into the command buffer
		MODE_NOCACHE, // as normal, the vertex cache off
		MODE_NORUNS,  // as normal, display list draws one at a time (as test build 34)
		MODE_COUNT
	};
	void requestSwitch(int which) { __sync_fetch_and_or(&mSwitchRequest, which); }
	void copy(const gpu::CopyInfo& info) override;
	void bpWritten(u32 reg, u32 value) override;

	const RendererStats& stats() const { return mStats; }

	// Parts of the state a BP register write can change (updateState redoes only those).
	enum { D_STATE = 1, D_TEV = 2, D_PARAMS = 4, D_TEX = 8, D_ALL = 15 };
	u32 framesPresented() const { return mFrame; }
	// The fragment program text for the current TEV set-up (debugging).
	const char* currentProgramText() const { return mProgram ? mProgram->prog.text.c_str() : ""; }

private:
	struct ProgramEntry {
		TevKey key;
		TevProgram prog;
		bool ok;
		u32 texCoordCount; // texture coordinates 0 .. n-1 cover those the program reads
	};
	struct TexKey {
		u32 address, width, height, format, tlutFormat;
		u64 tlutHash;
		bool operator<(const TexKey& o) const;
	};
	struct TexEntry {
		Texture* tex;
		u64 hash;
		u32 checkedFrame, usedFrame;
		u8 interval; // frames between checks
	};
	struct CopyEntry {
		Texture* tex;
		u32 width, height, format;
		u64 memHash;
		bool memKnown;
		u32 checkedFrame;
		u8 interval;
	};
	struct Marker {
		u32 fence;
		u64 pos;
	};
	struct Retired {
		Texture* tex;
		u32 fence;
	};
	struct Bound {
		const Texture* tex;
		Sampler sampler;
		bool valid;
	};
	struct TlutHash {
		u32 serial, tmem, bytes;
		u64 hash;
	};
	struct XfProgramEntry {
		XfKey key;
		VertexProgram prog;
		XfInfo info;
		bool ok;
	};
	// rows of the copy of XF matrix memory in the constants: 0-63 (lo), 64-65 (hi)
	struct Rows {
		u64 lo;
		u32 hi;
	};
	// Vertex arrays as the vertex cache knows them: the range its draws read, its hash, and a version that
	// changes when the memory does.
	struct ArrayRegion {
		ArrayRegion* next;
		u32 array, base, stride, lo, hi;
		u64 hash;
		bool hashValid;
		u32 version, checkedFrame, auditFrame;
		u8 interval;
	};
	enum { CACHE_ARRAYS = 6 };
	struct CacheArray {
		ArrayRegion* region;
		u32 base, version;
		u8 array, stride;
	};
	// A display list draw in the vertex cache: its vertices unpacked into the cache memory (from
	// firstVertex), its triangles (32-bit indices into the cache memory, at indexAt in the index memory:
	// the entries filled one after another have their triangles one after another, and draws in that order
	// are drawn as one), and what it was unpacked from. In the entries' memory: this (one 128-byte cache
	// line, what every draw from it reads), then a copy of the display list's vertex bytes, then the vertex
	// arrays it reads (arrayCap of them).
	struct CacheEntry {
		const u8* data;       // the draw's vertices in the display list
		XfProgramEntry* xp;
		CacheEntry* follow;   // the entry of the cached draw that came after this one last time
		CacheEntry* next;     // other entries for the same data (other programs)
		u64 rowsLo, nrowsLo;  // the rows of the matrices the vertices use (Rows, packed)
		u32 rowsHi, nrowsHi;
		u32 cp[7];            // vertex descriptor, the format's VAT A/B/C, MATINDEX A/B
		u32 gen, okFrame;     // okFrame: the last frame its vertex arrays were found unchanged
		u32 refillFrame;      // the last frame it was filled again (or, not cached, stopped being cached)
		u32 indexAt, indexCount, firstVertex;
		u16 count;
		u8 size, prim, arrays, arrayCap, refills, lines; // lines: 128-byte lines from the entry to its end
		bool cached;          // (false: changes too often, drawn from the ring)
		u8 followLines;       // (follow's lines: read ahead)
		u8 bytesOff;          // (the copy is as far into an 8-byte word as the display list's bytes: compared by words)
		u16 parts;            // draw commands (a draw run: gpu::Listener::drawRuns)
		u32 span() const { return count * size + 3u * (parts - 1u); } // (its bytes in the display list)
		u8* bytes() { return (u8*)this + sizeof(CacheEntry) + bytesOff; }
		CacheArray* arr() { return (CacheArray*)((u8*)this + sizeof(CacheEntry) + ((bytesOff + span() + 15) & ~15u)); }
		Rows rows() const { Rows r = { rowsLo, rowsHi }; return r; }
		Rows nrows() const { Rows r = { nrowsLo, nrowsHi }; return r; }
	};

	void log(const char* fmt, ...);

	// scratch memory (a ring, released by fences)
	void* ringAlloc(u32 bytes, u32 align);
	void mark();
	void release();
	void retire(Texture* t);

	// batches
	u8* batchVertices(u32 count, u32 stride, u32 texCoords, XfProgramEntry* xp, u32* first);
	void flushBatch();

	// state
	bool updateState();
	void bindProgram(const FragmentProgram& fp, const float (*params)[4]);
	const Texture* lookupTexture(u32 map, Sampler& sampler);
	// (checks of memory for changes: textures, frame buffer copies, vertex arrays)
	bool checkDue(u32 checkedFrame, u8 interval) const
	{
		if (mCheckEveryFrame) {
			return checkedFrame != mFrame; // (the test switch)
		}
		return mFrame - checkedFrame >= (interval ? interval : 1u);
	}
	void checked(u32& checkedFrame, u8& interval, bool changed, u32 address) const;
	const Texture* missingTexture();
	void invalidateDeviceState();

	// draws
	bool drawGpu(const gpu::DrawCall& call, XfProgramEntry* xp); // false: not possible, use drawCpu
	void drawRing(const gpu::DrawCall& call, XfProgramEntry* xp); // (unpacked into the ring: one part)
	void drawCpu(const gpu::DrawCall& call, u32 texCoords);
	XfProgramEntry* xfProgramFor(u32 texCoords);
	void uploadSetup();
	void matrixRowsUsed(const gpu::DrawCall& call, const XfInfo& info, Rows* rows, Rows* nrows);
	void uploadMatrices(const Rows& rows, const Rows& nrows);
	void setDeviceCull(u8 cull);

	// the vertex cache
	CacheEntry* cacheLookup(const gpu::DrawCall& call, XfProgramEntry* xp, CacheEntry** reuse);
	void cacheFollow(CacheEntry* e);
	CacheEntry* cacheFill(const gpu::DrawCall& call, XfProgramEntry* xp, CacheEntry* e);
	bool cacheValid(CacheEntry* e, const gpu::DrawCall& call);
	bool cacheVariant(const CacheEntry* e, const gpu::DrawCall& call);
	void checkRegion(ArrayRegion& r);
	ArrayRegion* regionFor(u32 array, u32 base, u32 stride);
	void* poolAlloc(u32 bytes, u32 align);
	void cacheReset();
	void batchCached(CacheEntry* e, XfProgramEntry* xp);

	// internal drawing
	void drawQuad(u32 targetWidth, u32 targetHeight, const float dst[4], const float uv[4], const Texture* source, bool linear,
	              const FragmentProgram& fp);
	void present(const gpu::CopyInfo& info);
	void copyToTexture(const gpu::CopyInfo& info);
	void clearEfb(const gpu::CopyInfo& info);
	void endFrame();
	void evictTextures();

	gpu::CommandProcessor& mCp;
	gpu::Memory& mMem;
	Device& mDev;
	void (*mLog)(const char*);
	RendererStats mStats;
	u32 mFrame;

	// TEV registers and constants (signed values), texture memory for palettes
	int mTevReg[4][4];
	int mKonst[4][4];
	float mRegF[NUM_TEV_PARAMS][4]; // the program parameters: registers, constants, alpha test references
	u8* mTmem;
	u32 mTlutSerial; // palette loads so far
	TlutHash mTlutCache[8];

	// ring
	u8* mScratch;
	u32 mScratchSize;
	u64 mHead, mTail, mLastMark;
	u32 mFenceCounter;
	u32 mFrameFence; // the fence after the last frame
	Vec<Marker> mMarkers;
	Vec<Retired> mRetired;

	// batch
	struct Batch {
		bool active;
		bool cached;   // vertices in the vertex cache (32-bit indices), not in the ring
		u8* base;
		u32 reserved, stride, texCoords, vertexCount;
		XfProgramEntry* xp; // the vertex program (NULL: vertices transformed on the CPU)
		Vec<unsigned short> indices;
		u32 at32, count32; // (vertex cache batches: their triangles, in the index memory)
		Rows rows, nrows; // the matrix rows its draws read (they must not change before it is drawn)
	} mBatch;

	// state
	u32 mDirtyBits; // D_*: what changed since the state was worked out
	bool mDrawable;
	bool mStateOk; // the scissor leaves something to draw
	u32 mViewport[6];
	RenderState mState;
	bool mStateValid;
	u32 mCull;
	Map<u32, Vec<ProgramEntry*> > mPrograms;
	ProgramEntry* mProgram;
	float mParams[NUM_TEV_PARAMS][4];
	bool mProgramBound;
	Bound mBound[8];
	Map<TexKey, TexEntry> mTextures;
	Map<u32, CopyEntry> mCopies;
	Texture* mMissing;
	u32 mTextureBytes;
	FragmentProgram mCopyPrograms[COPY_MODE_COUNT][2];
	bool mCopyProgramReady[COPY_MODE_COUNT][2];
	Vec<u32> mStaging; // texels decoded before they are copied to device memory
	// the transform set-up of the last draw (redone when XF memory or the vertex layout changes)
	gpu::XfSetup mXf;
	gpu::VertexLayout mXfLayout;
	u64 mXfSerial;
	bool mXfValid;

	// the transform on the RSX (xf_program.h)
	bool mGpuXf;
	Map<u32, Vec<XfProgramEntry*> > mXfPrograms;
	XfProgramEntry* mXfLookup; // the program for the current set-up (NULL: on the CPU)
	bool mXfLookupValid;
	const ProgramEntry* mXfLookupFp;
	u32 mXfLookupTexCoords;
	const gpu::VertexLayout* mXfLayoutAt; // (mXfLayout's source, while the processor's layouts are unchanged)
	u32 mXfLayoutVersion;
	const ProgramEntry* mXfLimitFp;       // the program mXf was limited (xfLimit) for
	XfProgramEntry* mBoundXp;             // the vertex program of the last batch drawn (NULL: pass-through)
	bool mBoundXpValid;
	float mSetup[XC_SETUP_COUNT][4]; // set-up constants on the device
	bool mSetupValid, mSetupDirty;
	// XF matrix memory in the constants (xf_program.h): the XF load each row was copied from
	u32 mXfWrites;      // XF loads so far
	Rows mStale, mNStale; // rows of the copy that XF loads have changed since they were copied
	u8 mIdentity[64];   // (the vertices keep the GameCube's matrix indices)

	// the vertex cache
	bool mCacheOn;
	volatile int mResetRequest;
	volatile int mSwitchRequest;
	u32 mMode;            // (the test switch: MODE_*)
	bool mCheckEveryFrame; // (the test switch: textures and vertex arrays checked every frame)
	int mFrontFaceSign;   // (the device's, from its self-test)
	CacheEntry* mLastEntry; // the entry of the last cached draw (its follow is the next one's)
	enum { WHY_GEN, WHY_CP, WHY_BYTES, WHY_BASE, WHY_VERSION };
	u32 mWhy, mWhyDetail; // (why cacheValid found the entry out of date: WHY_*, and which register or array)
	bool mNewVariant;     // (cacheLookup found entries for the draw, none for its registers and arrays)

	// audit frames (every AUDIT_FRAMES): everything the checks would skip is checked anyway, and what changed
	// without being noticed is logged (and corrected)
	bool mAudit;
	struct Audit {
		u32 draws, drawsWrong, textures, texturesChanged, regions, regionsChanged;
	} mAuditCounts;
	Vec<u8> mAuditVerts;
	Vec<u32> mAuditIdx;
	Vec<u8> mRunVerts;  // (a draw run's vertices without the commands' headers)
	const u8* runVertices(const gpu::DrawCall& call);
	void auditEntry(CacheEntry* e, const gpu::DrawCall& call, XfProgramEntry* xp);
	u32 mChainLength;   // (entries for the display list address of the last lookup)
	u8* mArena;         // cache memory for vertices (the end of the scratch memory)
	u32 mArenaSize, mArenaHead, mArenaGen;
	u32* mIdx;          // ... and for their triangles
	u32 mIdxSize, mIdxHead; // (indices)
	CacheEntry** mTable; // by display list address (open addressing)
	u32 mTableSlots, mTableUsed;
	Vec<u8*> mPool;      // CPU memory for entries (freed together by cacheReset)
	u32 mPoolUsed, mPoolBytes;
	ArrayRegion* mRegions; // (in the entries' memory: freed by cacheReset)
};

} // namespace gfx

#endif
