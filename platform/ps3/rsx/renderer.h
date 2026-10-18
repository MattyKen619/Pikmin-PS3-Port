// PS3 port: draws the GameCube's graphics commands with a modern-ish GPU (the PS3's RSX, through the
// Device interface in device.h).
//
// The command processor (platform/ps3/gpu/gpu_cp.h) reports each draw. The renderer
//   - has the RSX transform and light the vertices: the XF set-up becomes a vertex program
//     (xf_program.h), the matrices the vertices use go into a palette of vertex program constants, and the
//     CPU only unpacks the vertices into the RSX's memory (CommandProcessor::gatherVertices). Set-ups the
//     vertex programs do not cover (emboss texture coordinates), or a device whose vertex programs failed
//     the start-up self-test, are transformed on the CPU instead (gpu_xf.h, the same code as the host's
//     reference renderer), with back faces removed the way the GameCube does;
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
	u64 gpuDraws, cpuDraws, xfPrograms, xfProgramErrors, paletteRows, setupUploads, vertexProgramLoads;
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

	// (with the transform on the RSX, vertices are unpacked straight into its memory: no decoding)
	gpu::VertexDetail vertexDetail() override { return mGpuXf ? gpu::VERTICES_ARRAYS : gpu::VERTICES_DECODED; }
	void draw(const gpu::DrawCall& call) override;
	void xfWritten(u32 address, u32 count) override;
	// Transform on the RSX when it can (the default when the device's vertex programs work), or always on
	// the CPU.
	void setGpuTransform(bool on) { mGpuXf = on && mDev.vertexProgramsWork() && mDev.frontFaceSign() != 0; }
	bool gpuTransform() const { return mGpuXf; }
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
	struct Slot {
		u32 lap, serial;
		u8 offset, rows;
	};
	struct FixedSlot {
		bool valid;
		u32 row, rows, serial;
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
	bool checkDue(u32 checkedFrame, u8 interval) const;
	void checked(u32& checkedFrame, u8& interval, bool changed, u32 address) const;
	const Texture* missingTexture();
	void invalidateDeviceState();

	// draws
	bool drawGpu(const gpu::DrawCall& call, XfProgramEntry* xp); // false: not possible, use drawCpu
	void drawCpu(const gpu::DrawCall& call, u32 texCoords);
	XfProgramEntry* xfProgramFor(u32 texCoords);
	u8 paletteSlot(Slot* slots, u32 row, u32 rows);
	void fixedMatrix(FixedSlot& f, u32 base, u32 row, u32 rows);
	void matrixRows(u32 row, u32 rows, float data[6][4]);
	void setDeviceCull(u8 cull);

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
		u8* base;
		u32 reserved, stride, texCoords, vertexCount;
		XfProgramEntry* xp; // the vertex program (NULL: vertices transformed on the CPU)
		Vec<unsigned short> indices;
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
	// the matrix palette: XF matrix rows copied into constants, found again while unchanged
	u32 mXfWrites;      // XF loads so far
	u32 mRowWrite[64];  // the last load that reached each matrix row (XF 0x000-0x0FF, 4 words a row)
	u32 mNrmWrite[48];  // the same for the normal matrices (XF 0x400-0x45F, 3 words a row)
	Slot mPosSlot[64], mTexSlot[64];
	u32 mPalHead, mPalLap; // rows used, times the palette was started again
	u8 mPosMap[64], mTexMap[64];
	FixedSlot mFixedPos, mFixedTex[8]; // (without relative addressing)
};

} // namespace gfx

#endif
