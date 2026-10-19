// PS3 port: the renderer's Device (device.h) on the RSX, through PSL1GHT's librsx. Built with the PS3
// SDK headers (backend flags).
//
// The command buffer, display buffers and RSX-mapped main memory come from the text console when the
// renderer takes the display over (ps3b_console_take_display). Draw data (vertices, fragment program
// microcode) is in that main memory; textures, the embedded frame buffer and its depth buffer are in RSX
// local memory. Indices are sent inline in the command buffer.

#ifndef PS3_RSX_DEVICE_RSX_H
#define PS3_RSX_DEVICE_RSX_H

#include "device.h"

namespace gfx {

class RsxDevice : public Device {
public:
	RsxDevice();
	// Takes the display over and sets the RSX up. False (and a log line) if something failed.
	bool init();
	// Draws test patterns and reads them back: logs what works. Leaves the frame buffer cleared.
	void selfTest();
	// Stops sending commands (process exit): waits for the RSX to finish, with a time limit.
	static void stopForExit();

	u8* scratchBase() override { return mScratch; }
	u32 scratchSize() override { return mScratchSize; }
	void writeFence(u32 value) override;
	u32 fenceReached() override;
	void kick() override;
	void pause() override;

	Texture* createTexture(u32 width, u32 height) override;
	void destroyTexture(Texture* t) override;
	const Texture* efbColor() override { return &mEfbTexture; }
	void texturesChanged() override { mInvalidateTextures = true; }

	void setTarget(const Texture* texture) override;
	bool beginDisplay(u32* width, u32* height) override;
	void endDisplay() override;

	void setState(const RenderState& s) override;
	void setProgram(const void* ucode, const FragmentProgram& fp) override;
	void setTexture(u32 unit, const Texture* t, const Sampler& s) override;
	void setVertices(const void* base, u32 stride, u32 texCoords) override;
	void setVertexProgram(const VertexProgram* program) override;
	void setVertexConstants(u32 first, u32 count, const float* values) override;
	void setVertexFormat(const void* base, const VertexFormat& format) override;
	int frontFaceSign() override { return mFrontFaceSign; }
	bool vertexProgramsWork() override { return mVertexProgramsWork; }
	u32 vertexIndexType() override { return mVertexIndexType; }
	void drawTriangles(const unsigned short* indices, u32 count) override;
	void drawTriangles32(const u32* indices, u32 count) override;
	void drawTriangles32Inline(const u32* indices, u32 count) override;
	void sleepUs(u32 us) override;
	void setProgramCacheDrop(bool on) override { mProgramCacheDrop = on; }
	bool index32Works() override { return mIndex32Works; }
	void clear(const u16 rect[4], u32 argb, u32 depth, bool color, bool alpha, bool depthToo) override;
	bool readTarget(u32* out, u32 x, u32 y, u32 width, u32 height) override;

	u32 displayWidth() const { return mDispWidth; }
	u32 displayHeight() const { return mDispHeight; }
	u32 flips() const { return mFlips; }
	u32 localMemoryUsed() const { return mLocalUsed; }

private:
	void surface(u32 colorOffset, u32 colorPitch, u32 width, u32 height, u32 depthOffset, u32 depthPitch, bool argb);
	bool waitLabel(u32 value, u32 timeoutUs);
	void applyState(const RenderState& s, bool all);
	void testVertexPrograms(u8* mem, const RenderState& rs);
	void checkStop();

	void* mCtx; // gcmContextData*
	u8* mScratch;
	u32 mScratchSize;
	// display
	u32 mDispWidth, mDispHeight, mDispPitch;
	u32 mDispOffset[2];
	u32* mDispBuffer[2];
	u32 mDispNext;
	bool mFlipped;
	u32 mDispDepthOffset;
	u32 mFlips;
	// embedded frame buffer
	u32* mEfbColorMem;
	u32 mEfbColorOffset, mEfbDepthOffset;
	Texture mEfbTexture;
	// current target
	const u32* mTargetMem;
	u32 mTargetPitch, mTargetWidth, mTargetHeight;
	// state
	RenderState mState;
	bool mStateValid;
	u32 mBoundTexCoords;
	bool mInvalidateTextures;
	bool mProgramCacheDrop; // (setProgramCacheDrop)
	u32 mReadLabelValue;
	u32 mLocalUsed;
	const VertexProgram* mVertexProgram; // bound (NULL: the pass-through program)
	bool mVertexProgramValid;
	int mFrontFaceSign;       // (self-test)
	bool mVertexProgramsWork; // (self-test)
	u32 mVertexIndexType;     // (self-test)
	bool mIndex32Works;       // (self-test)
};

} // namespace gfx

#endif
