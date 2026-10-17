// PS3 port, host tools: a software renderer for the GameCube's graphics processor, driven by the port's
// command processor (platform/ps3/gpu/gpu_cp.h). It turns a frame trace into the picture the GameCube
// would have shown: vertex transform and lighting (XF), triangle setup and rasterization, texturing,
// the TEV color combiners, alpha test, depth test and blending (BP), and copies out of the embedded frame
// buffer (to the display, and to textures that later draws sample).
//
// It is the reference for the PS3's own renderer: correctness first, speed second. Not yet supported:
// fog, indirect textures, lines and points, dual (post-transform) texture matrices, anti-aliasing, mipmaps
// (level 0 is used). Each is counted (Stats) so a frame shows what it misses.

#ifndef GX_HOST_SW_RENDER_H
#define GX_HOST_SW_RENDER_H

#include "../../../platform/ps3/gpu/gpu_cp.h"

#include <map>
#include <vector>

namespace swr {

using gpu::u16;
using gpu::u32;
using gpu::u64;
using gpu::u8;

enum { EFB_WIDTH = 640, EFB_HEIGHT = 528 };

struct Stats {
	u64 draws, triangles, culled, clipped, pixels, pixelsWritten;
	u64 lines, points, fogDraws, indirectDraws, dualTexDraws, embossTexgens, missingTextures, copyTextureUses;
	u64 tevCompareStages;
};

// A texture made by an EFB copy (stored decoded; later draws that sample its address use it).
struct CopyTexture {
	u32 width, height, format;
	std::vector<u32> rgba; // 0xRRGGBBAA
};

class Renderer : public gpu::Listener {
public:
	Renderer(gpu::CommandProcessor& cp, gpu::Memory& memory);

	// Before replaying a trace: take the TEV color/konst registers and TLUT state from the start state.
	void begin();

	gpu::VertexDetail vertexDetail() override { return gpu::VERTICES_DECODED; }
	void draw(const gpu::DrawCall& call) override;
	void copy(const gpu::CopyInfo& info) override;
	void bpWritten(u32 reg, u32 value) override;

	// The last copy to the display (RGB, width x height), and the embedded frame buffer.
	const std::vector<u8>& frame() const { return mFrame; }
	u32 frameWidth() const { return mFrameW; }
	u32 frameHeight() const { return mFrameH; }
	u32 framesCopied() const { return mFrames; }
	const u32* efbColor() const { return mColor; }
	const Stats& stats() const { return mStats; }
	const std::map<u32, CopyTexture>& copyTextures() const { return mCopies; }

	// Draws to skip (debugging): draws numbered first..last (1-based) are not drawn.
	void skipDraws(u64 first, u64 last)
	{
		mSkipFirst = first;
		mSkipLast  = last;
	}

	struct OutVertex {
		float clip[4];      // clip-space position
		float color[2][4];  // channel colors 0-1
		float tex[8][3];    // s, t, q
	};

private:
	void transform(const gpu::Vertex& in, const gpu::VertexLayout& layout, OutVertex& out);
	void triangle(const OutVertex& a, const OutVertex& b, const OutVertex& c);
	void rasterize(const OutVertex* v[3]);
	void shade(int x, int y, float z, const float* color0, const float* color1, const float (*tex)[3]);
	void sample(int map, float s, float t, float out[4]);

	gpu::CommandProcessor& mCp;
	gpu::Memory& mMem;
	u32 mColor[EFB_WIDTH * EFB_HEIGHT]; // 0xRRGGBBAA
	u32 mDepth[EFB_WIDTH * EFB_HEIGHT];
	u8 mTmem[0x100000];
	int mTevReg[4][4];   // PREV, C0, C1, C2 (signed 11-bit)
	int mKonst[4][4];    // K0-K3
	std::map<u32, CopyTexture> mCopies;
	std::vector<u8> mFrame;
	u32 mFrameW, mFrameH, mFrames;
	Stats mStats;
	u64 mSkipFirst, mSkipLast;
	u64 mDrawNumber;

	// per-draw state (cached from the registers)
	int mCull;
	int mScissor[4]; // x0, y0, x1, y1 inclusive
};

// Memory that textures were decoded from changed (texture address space, 26-bit).
void invalidateTextures(u32 address, u32 size);

// Writes an RGB image as PNG.
bool writePng(const char* path, const u8* rgb, u32 width, u32 height);

} // namespace swr

#endif
