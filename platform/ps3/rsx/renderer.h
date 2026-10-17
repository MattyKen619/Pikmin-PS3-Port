// PS3 port: draws the GameCube's graphics commands with a modern-ish GPU (the PS3's RSX, through the
// Device interface in device.h).
//
// The command processor (platform/ps3/gpu/gpu_cp.h) reports each draw with its decoded vertices. The
// renderer
//   - transforms and lights the vertices on the CPU (gpu_xf.h, the same code as the host's reference
//     renderer), and removes back faces the way the GameCube does;
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

#include <map>
#include <vector>

namespace gfx {

typedef unsigned long long u64;

struct RendererStats {
	u64 draws, batches, triangles, culled, vertices, skippedDraws, lineDraws, pointDraws;
	u64 programs, programErrors, programBinds, textureUploads, textureUploadBytes, missingTextures;
	u64 copies, copyUses, presents, clears, fenceWaits;
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

	gpu::VertexDetail vertexDetail() override { return gpu::VERTICES_DECODED; }
	void draw(const gpu::DrawCall& call) override;
	void copy(const gpu::CopyInfo& info) override;
	void bpWritten(u32 reg, u32 value) override;

	const RendererStats& stats() const { return mStats; }
	u32 framesPresented() const { return mFrame; }
	// The fragment program text for the current TEV set-up (debugging).
	const char* currentProgramText() const { return mProgram ? mProgram->prog.text.c_str() : ""; }

private:
	struct ProgramEntry {
		TevKey key;
		TevProgram prog;
		bool ok;
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
	};
	struct CopyEntry {
		Texture* tex;
		u32 width, height, format;
		u64 memHash;
		bool memKnown;
		u32 checkedFrame;
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

	void log(const char* fmt, ...);

	// scratch memory (a ring, released by fences)
	void* ringAlloc(u32 bytes, u32 align);
	void mark();
	void release();
	void retire(Texture* t);

	// batches
	u8* batchVertices(u32 count, u32 texCoords, u32* first);
	void flushBatch();

	// state
	bool updateState();
	void bindProgram(const FragmentProgram& fp, const float (*params)[4]);
	const Texture* lookupTexture(u32 map, Sampler& sampler);
	const Texture* missingTexture();
	void invalidateDeviceState();

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
	u8* mTmem;

	// ring
	u8* mScratch;
	u32 mScratchSize;
	u64 mHead, mTail, mLastMark;
	u32 mFenceCounter;
	std::vector<Marker> mMarkers;
	std::vector<Retired> mRetired;

	// batch
	struct Batch {
		bool active;
		u8* base;
		u32 reserved, stride, texCoords, vertexCount;
		std::vector<unsigned short> indices;
	} mBatch;

	// state
	bool mDirty;      // BP registers or the frame changed since the state was worked out
	bool mDrawable;
	u32 mViewport[6];
	RenderState mState;
	bool mStateValid;
	u32 mCull;
	std::map<u32, std::vector<ProgramEntry*> > mPrograms;
	ProgramEntry* mProgram;
	float mParams[NUM_TEV_PARAMS][4];
	bool mProgramBound;
	Bound mBound[8];
	std::map<TexKey, TexEntry> mTextures;
	std::map<u32, CopyEntry> mCopies;
	Texture* mMissing;
	u32 mTextureBytes;
	FragmentProgram mCopyPrograms[COPY_MODE_COUNT][2];
	bool mCopyProgramReady[COPY_MODE_COUNT][2];
	std::vector<u32> mStaging; // texels decoded before they are copied to device memory
	// the transform set-up of the last draw (redone when XF memory or the vertex layout changes)
	gpu::XfSetup mXf;
	gpu::VertexLayout mXfLayout;
	u64 mXfSerial;
	bool mXfValid;
};

} // namespace gfx

#endif
