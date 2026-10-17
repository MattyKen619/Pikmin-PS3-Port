// PS3 port: what the renderer (renderer.h) needs from the graphics chip. On the PS3 this is the RSX
// (device_rsx.cpp, through PSL1GHT's librsx); the host tools implement it in software to check the
// renderer against the reference software renderer.
//
// Conventions:
//   - Render targets and textures are 32-bit A8R8G8B8 (texels written by the CPU as 0xAARRGGBB words),
//     linear (not swizzled), rows `pitch` bytes apart.
//   - The embedded frame buffer (EFB) is 640 x 528 with a 24-bit depth buffer, origin at the top left.
//   - Vertices: position (4 floats, clip space) at offset 0, color 0 and color 1 (4 bytes each, R G B A,
//     0-255 = 0-1) at 16 and 20, texture coordinate n (3 floats: s, t, q) at 24 + 12n. The vertex program
//     passes them through; texture coordinates reach the fragment program as (s, t, q, q).
//   - Draw data (vertices, fragment program microcode) is placed by the renderer in the device's scratch
//     memory; fences tell it when the chip is done with it.
//
// Portable C++.

#ifndef PS3_RSX_DEVICE_H
#define PS3_RSX_DEVICE_H

#include "fp_assembler.h"

namespace gfx {

typedef unsigned char u8;

enum { EFB_WIDTH = 640, EFB_HEIGHT = 528 };

struct Texture {
	u32 width, height;
	u32 pitch;     // bytes per row
	u32* texels;   // CPU address of the first texel
	u32 bytes;     // memory used
	void* impl;
};

enum BlendFactor {
	BLEND_ZERO,
	BLEND_ONE,
	BLEND_SRC_COLOR,
	BLEND_INV_SRC_COLOR,
	BLEND_DST_COLOR,
	BLEND_INV_DST_COLOR,
	BLEND_SRC_ALPHA,
	BLEND_INV_SRC_ALPHA,
	BLEND_DST_ALPHA,
	BLEND_INV_DST_ALPHA,
};

struct RenderState {
	u8 blend;          // 0 off, 1 add (src * srcFactor + dst * dstFactor), 2 reverse subtract (dst - src)
	u8 srcFactor, dstFactor;
	u8 logicOp;        // 0-15 (GX order: CLEAR, AND, REVAND, COPY, INVAND, NOOP, XOR, OR, NOR, EQUIV, INV,
	                   // REVOR, INVCOPY, INVOR, NAND, SET); only when blend == 0 and logicEnable
	u8 logicEnable;
	u8 depthTest;      // compare and (if depthWrite) update the depth buffer
	u8 depthFunc;      // GX compare function 0-7: NEVER LESS EQUAL LEQUAL GREATER NEQUAL GEQUAL ALWAYS
	u8 depthWrite;
	u8 colorWrite, alphaWrite;
	u16 scissor[4];    // x, y, width, height (inside the target)
	float viewportScale[3], viewportOffset[3];

	bool operator==(const RenderState& o) const
	{
		return blend == o.blend && srcFactor == o.srcFactor && dstFactor == o.dstFactor && logicOp == o.logicOp
		    && logicEnable == o.logicEnable && depthTest == o.depthTest && depthFunc == o.depthFunc
		    && depthWrite == o.depthWrite && colorWrite == o.colorWrite && alphaWrite == o.alphaWrite
		    && scissor[0] == o.scissor[0] && scissor[1] == o.scissor[1] && scissor[2] == o.scissor[2]
		    && scissor[3] == o.scissor[3] && viewportScale[0] == o.viewportScale[0] && viewportScale[1] == o.viewportScale[1]
		    && viewportScale[2] == o.viewportScale[2] && viewportOffset[0] == o.viewportOffset[0]
		    && viewportOffset[1] == o.viewportOffset[1] && viewportOffset[2] == o.viewportOffset[2];
	}
	bool operator!=(const RenderState& o) const { return !(*this == o); }
};

enum Wrap { WRAP_CLAMP = 0, WRAP_REPEAT = 1, WRAP_MIRROR = 2 };

struct Sampler {
	u8 wrapS, wrapT;
	u8 linearMin, linearMag;
	bool operator==(const Sampler& o) const
	{
		return wrapS == o.wrapS && wrapT == o.wrapT && linearMin == o.linearMin && linearMag == o.linearMag;
	}
};

enum { VERTEX_BASE_BYTES = 24, VERTEX_TEX_BYTES = 12 };

class Device {
public:
	virtual ~Device() { }

	// ---- memory the chip reads draw data from
	virtual u8* scratchBase() = 0;
	virtual u32 scratchSize() = 0;

	// ---- fences: the chip records `value` once everything submitted before is finished
	virtual void writeFence(u32 value) = 0;
	virtual u32 fenceReached() = 0;
	virtual void kick() = 0;               // start the chip on what was submitted
	virtual void pause() { }               // sleep briefly (waiting for a fence)

	// ---- textures (also usable as render targets)
	virtual Texture* createTexture(u32 width, u32 height) = 0; // NULL: out of memory
	virtual void destroyTexture(Texture* t) = 0;               // (the chip is done with it)
	virtual const Texture* efbColor() = 0;                     // the EFB's color buffer, to read as a texture
	virtual void texturesChanged() = 0;    // texels were written (by the CPU or by drawing): drop cached texels

	// ---- render targets
	virtual void setTarget(const Texture* texture) = 0;        // NULL: the EFB
	virtual bool beginDisplay(u32* width, u32* height) = 0;    // target the next display buffer
	virtual void endDisplay() = 0;                             // show it (at the next vertical blank)

	// ---- drawing
	virtual void setState(const RenderState& s) = 0;
	virtual void setProgram(const void* ucode, const FragmentProgram& fp) = 0; // microcode in scratch memory
	virtual void setTexture(u32 unit, const Texture* t, const Sampler& s) = 0;
	virtual void setVertices(const void* base, u32 stride, u32 texCoords) = 0; // in scratch memory
	virtual void drawTriangles(const unsigned short* indices, u32 count) = 0;  // indices into the vertices
	// Clears a rectangle of the current target (the scissor is not used).
	virtual void clear(const u16 rect[4], u32 argb, u32 depth, bool color, bool alpha, bool depthToo) = 0;

	// ---- checking
	// Copies a rectangle of the current target (0xAARRGGBB), after waiting for the chip.
	virtual bool readTarget(u32* out, u32 x, u32 y, u32 width, u32 height) = 0;
};

} // namespace gfx

#endif
