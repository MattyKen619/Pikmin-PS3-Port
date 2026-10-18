// PS3 port: the GameCube's transform unit (XF) in software: vertex positions to clip space, normals,
// lighting (two color channels, eight lights) and texture coordinate generation, from the XF memory and
// registers the command processor keeps (gpu::State::xf).
//
// Shared by the PS3 renderer (platform/ps3/rsx) and the host tools' software renderer, so both transform
// vertices identically. Register layouts follow the hardware as the GX library (src/gx) programs it.
//
// Portable C++ (no PS3 or game headers).

#ifndef PS3_GPU_XF_H
#define PS3_GPU_XF_H

#include "gpu_cp.h"

namespace gpu {

struct XfVertex {
	float pos[4];      // clip space
	float color[2][4]; // channel colors, 0-1
	float tex[8][3];   // s, t, q
};

// What XF does for one draw, read from the registers once (xfPrepare) and applied to each vertex.
struct XfSetup {
	const u32* xf;
	const VertexLayout* layout;
	float proj[6];
	bool ortho;
	u32 numChans;
	u32 numTexGens;
	bool dual;      // post-transform (dual) texture matrices
	bool needNormal;
	struct Part {    // the color or the alpha half of a channel
		bool matFromVertex, ambFromVertex, lighting;
		u32 lightMask, diffuse;
		bool attn, spot;
		float mat[4], amb[4];
	} part[2][2];    // [channel][0: color, 1: alpha]
	struct Light {
		float color[4], A[3], K[3], pos[3], dir[3];
	} light[8];
	struct TexGen {
		u32 type, row, from;
		bool stq, abc1, normalize;
		const float* post; // post-transform matrix (dual), or NULL
	} texgen[8];
	bool emboss;     // an emboss (bump) texgen is used: approximated
	// What xfTransform works out (xfPrepare: everything). A renderer that needs less can lower these.
	u32 chanMask;    // color channels (bit per channel; others come out 0)
	u32 texLimit;    // texture coordinates 0 .. texLimit-1 (others come out (0, 0, 1))
};

// Lowers the work to the channels and texture coordinates a draw uses. (An emboss texgen needs the
// coordinates before it: texLimit can then come out higher than texCoords.)
void xfLimit(XfSetup& setup, u32 chanMask, u32 texCoords);

void xfPrepare(const u32* xf, const VertexLayout& layout, XfSetup& setup);
void xfTransform(const XfSetup& setup, const Vertex& in, XfVertex& out);

} // namespace gpu

#endif
