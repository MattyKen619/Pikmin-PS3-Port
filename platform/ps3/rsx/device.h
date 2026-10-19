// PS3 port: what the renderer (renderer.h) needs from the graphics chip. On the PS3 this is the RSX
// (device_rsx.cpp, through PSL1GHT's librsx); the host tools implement it in software to check the
// renderer against the reference software renderer.
//
// Conventions:
//   - Render targets and textures are 32-bit A8R8G8B8 (texels written by the CPU as 0xAARRGGBB words),
//     linear (not swizzled), rows `pitch` bytes apart.
//   - The embedded frame buffer (EFB) is 640 x 528 with a 24-bit depth buffer, origin at the top left.
//   - Vertices come in two kinds:
//       * transformed (setVertices; the device's built-in pass-through vertex program): position (4
//         floats, clip space), color 0 and color 1 (4 floats each, 0-1), texture coordinate n (3 floats:
//         s, t, q) at VERTEX_BASE_BYTES + VERTEX_TEX_BYTES * n. Texture coordinates reach the fragment
//         program as (s, t, q, q).
//       * raw, for a vertex program of the renderer's (setVertexProgram, setVertexConstants and
//         setVertexFormat): attributes as the format describes them.
//   - Draw data (vertices, fragment program microcode) is placed by the renderer in the device's scratch
//     memory; fences tell it when the chip is done with it.
//
// Portable C++.

#ifndef PS3_RSX_DEVICE_H
#define PS3_RSX_DEVICE_H

#include "fp_assembler.h"
#include "vp_builder.h"

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
	u8 cullFace;       // 0 none, CULL_FRONT or CULL_BACK (the device's front faces: see Device::frontFaceSign)

	bool operator==(const RenderState& o) const
	{
		return blend == o.blend && srcFactor == o.srcFactor && dstFactor == o.dstFactor && logicOp == o.logicOp
		    && logicEnable == o.logicEnable && depthTest == o.depthTest && depthFunc == o.depthFunc
		    && depthWrite == o.depthWrite && colorWrite == o.colorWrite && alphaWrite == o.alphaWrite
		    && scissor[0] == o.scissor[0] && scissor[1] == o.scissor[1] && scissor[2] == o.scissor[2]
		    && scissor[3] == o.scissor[3] && viewportScale[0] == o.viewportScale[0] && viewportScale[1] == o.viewportScale[1]
		    && viewportScale[2] == o.viewportScale[2] && viewportOffset[0] == o.viewportOffset[0]
		    && viewportOffset[1] == o.viewportOffset[1] && viewportOffset[2] == o.viewportOffset[2] && cullFace == o.cullFace;
	}
	bool operator!=(const RenderState& o) const { return !(*this == o); }
};

enum { CULL_NONE = 0, CULL_FRONT = 1, CULL_BACK = 2 };

enum Wrap { WRAP_CLAMP = 0, WRAP_REPEAT = 1, WRAP_MIRROR = 2 };

struct Sampler {
	u8 wrapS, wrapT;
	u8 linearMin, linearMag;
	bool operator==(const Sampler& o) const
	{
		return wrapS == o.wrapS && wrapT == o.wrapT && linearMin == o.linearMin && linearMag == o.linearMag;
	}
};

// Vertex layout (gpu::XfVertex's, cut after the texture coordinates a batch uses): position 4 floats,
// color channels 0 and 1 as 4 floats each (0-1), then 3 floats (s, t, q) per texture coordinate.
enum { VERTEX_BASE_BYTES = 48, VERTEX_TEX_BYTES = 12 };

// Raw vertices for the renderer's vertex programs: up to 16 attributes (the vertex program's inputs).
enum AttribType {
	ATTRIB_F32     = 2, // 32-bit floats
	ATTRIB_U8_NORM = 4, // bytes, 0-255 read as 0-1
	ATTRIB_U8      = 7, // bytes, read as 0-255
};
struct VertexAttrib {
	u8 components; // 1-4; 0: not used (the program reads (0, 0, 0, 1))
	u8 type;       // AttribType
	u16 offset;    // in the vertex
};
struct VertexFormat {
	VertexAttrib attr[16];
	u32 stride;
};

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
	// A vertex program of the renderer's (NULL: the pass-through program setVertices needs). The program
	// must stay unchanged while it is set.
	virtual void setVertexProgram(const VertexProgram* program) = 0;
	// Vertex program constants c[first .. first + count - 1] (4 floats each). In order with the draws:
	// draws submitted before keep the values they were drawn with.
	virtual void setVertexConstants(u32 first, u32 count, const float* values) = 0;
	virtual void setVertexFormat(const void* base, const VertexFormat& format) = 0; // in scratch memory
	// Which triangles the device calls front-facing, by the sign of their screen area as the GameCube
	// computes it (the homogeneous determinant of the clip-space positions times the viewport's x and y
	// scales): +1 positive, -1 negative, 0 unknown (the renderer then removes back faces itself).
	virtual int frontFaceSign() = 0;
	// The renderer's vertex programs work on this device (checked at start-up on the PS3).
	virtual bool vertexProgramsWork() = 0;
	// How vertices can pick constants (relative addressing): the attribute type of the index values that
	// works (ATTRIB_U8, ATTRIB_U8_NORM or ATTRIB_F32), or 0 if relative addressing does not work.
	virtual u32 vertexIndexType() = 0;
	virtual void drawTriangles(const unsigned short* indices, u32 count) = 0;  // indices into the vertices
	// The same with 32-bit indices that are in scratch memory (the chip reads them there): vertices anywhere
	// in the renderer's vertex cache. Only if index32Works.
	virtual void drawTriangles32(const u32* indices, u32 count) = 0;
	virtual bool index32Works() = 0;
	// (test modes) The same with the indices copied into the command buffer, and a pause of the calling
	// thread.
	virtual void drawTriangles32Inline(const u32* indices, u32 count) { drawTriangles32(indices, count); }
	virtual void sleepUs(u32 us) { (void)us; }
	// Whether setProgram drops the chip's texture cache first (on by default): the RSX reads fragment
	// programs through it, and a new program written where it has read before may come from the cache.
	virtual void setProgramCacheDrop(bool on) { (void)on; }
	// Clears a rectangle of the current target (the scissor is not used).
	virtual void clear(const u16 rect[4], u32 argb, u32 depth, bool color, bool alpha, bool depthToo) = 0;

	// ---- checking
	// Copies a rectangle of the current target (0xAARRGGBB), after waiting for the chip.
	virtual bool readTarget(u32* out, u32 x, u32 y, u32 width, u32 height) = 0;
};

} // namespace gfx

#endif
