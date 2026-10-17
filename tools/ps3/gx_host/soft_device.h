// PS3 port, host tools: a software stand-in for the RSX behind the renderer's Device interface
// (platform/ps3/rsx/device.h). It runs the renderer's fragment programs from their microcode (an
// interpreter for the NV40 instructions the TEV programs use), so the renderer's output - programs,
// state, batches, copies - can be checked on the host against the reference software renderer.
//
// The display is 640 x 480, so a presented frame matches the reference renderer's frame pixel for pixel.

#ifndef GX_HOST_SOFT_DEVICE_H
#define GX_HOST_SOFT_DEVICE_H

#include "../../../platform/ps3/rsx/device.h"

#include <vector>

class SoftDevice : public gfx::Device {
public:
	SoftDevice();
	~SoftDevice();

	gfx::u8* scratchBase() override { return &mScratch[0]; }
	gfx::u32 scratchSize() override { return (gfx::u32)mScratch.size(); }
	void writeFence(gfx::u32 value) override { mFence = value; }
	gfx::u32 fenceReached() override { return mFence; }
	void kick() override { }

	gfx::Texture* createTexture(gfx::u32 width, gfx::u32 height) override;
	void destroyTexture(gfx::Texture* t) override;
	const gfx::Texture* efbColor() override { return &mEfbTexture; }
	void texturesChanged() override { }

	void setTarget(const gfx::Texture* texture) override;
	bool beginDisplay(gfx::u32* width, gfx::u32* height) override;
	void endDisplay() override { mFrames++; }

	void setState(const gfx::RenderState& s) override { mState = s; }
	void setProgram(const void* ucode, const gfx::FragmentProgram& fp) override;
	void setTexture(gfx::u32 unit, const gfx::Texture* t, const gfx::Sampler& s) override;
	void setVertices(const void* base, gfx::u32 stride, gfx::u32 texCoords) override;
	void drawTriangles(const unsigned short* indices, gfx::u32 count) override;
	void clear(const gfx::u16 rect[4], gfx::u32 argb, gfx::u32 depth, bool color, bool alpha, bool depthToo) override;
	bool readTarget(gfx::u32* out, gfx::u32 x, gfx::u32 y, gfx::u32 width, gfx::u32 height) override;

	// results
	const std::vector<gfx::u32>& display() const { return mDisplay; } // 0xAARRGGBB
	gfx::u32 displayWidth() const { return 640; }
	gfx::u32 displayHeight() const { return 480; }
	gfx::u32 framesShown() const { return mFrames; }
	const gfx::u32* efb() const { return &mEfb[0]; }

	struct Stats {
		unsigned long long triangles, pixels, killed, written, programErrors;
	};
	const Stats& stats() const { return mStats; }

private:
	struct Vert {
		float pos[4];
		float in[16][4]; // fragment program inputs: 1 COL0, 2 COL1, 4-11 TEX0-7
	};
	void fetch(unsigned short index, Vert& v);
	void triangle(const Vert& a, const Vert& b, const Vert& c);
	void rasterize(const Vert* v[3]);
	bool runProgram(const float in[16][4], float out[4]);
	void sample(gfx::u32 unit, float s, float t, float out[4]);
	void pixel(int x, int y, float z, const float in[16][4]);

	std::vector<gfx::u8> mScratch;
	gfx::u32 mFence;
	std::vector<gfx::u32> mEfb, mDepth, mDisplay;
	gfx::Texture mEfbTexture;
	// current target
	gfx::u32* mColor;
	gfx::u32 mPitch; // texels
	gfx::u32 mWidth, mHeight;
	bool mHasDepth;
	gfx::u32 mFrames;

	gfx::RenderState mState;
	const gfx::u32* mUcode;
	gfx::FragmentProgram mFp;
	const gfx::Texture* mTex[16];
	gfx::Sampler mSampler[16];
	const gfx::u8* mVerts;
	gfx::u32 mStride, mTexCoords;
	Stats mStats;
};

#endif
