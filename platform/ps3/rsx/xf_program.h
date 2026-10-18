// PS3 port: the GameCube's transform unit (XF) as RSX vertex programs.
//
// For each distinct XF set-up (which color channels are lit and how, which lights, which texture
// coordinates are generated and from what) the renderer builds a vertex program (vp_builder.h) that does
// what gpu_xf.cpp does on the CPU: position and normal transforms, lighting, texture coordinate
// generation and the post-transform (dual) texture matrices. The CPU then only unpacks the vertices
// (CommandProcessor::gatherVertices) and loads matrices into constants.
//
// Constants (4 floats each):
//   c[0]      (0, 1, 0.5, 2)                     c[1]  (1e-30, 0, 0, 1)
//   c[2..5]   projection matrix rows             c[6..9] material 0, ambient 0, material 1, ambient 1
//   c[10..57] lights, 6 rows each: color, attenuation A (x y z), K (x y z), position, direction,
//             normalized position (the specular half-angle vector)
//   c[58..81] post-transform matrices, 3 rows per texture coordinate
//   c[82]     (255, 0.5, 0, 0)
//   c[83..148]  XF matrix memory: c[83 + r] = row r (4 floats), r = 0-63 (64, 65: zero)
//   c[149..214] the normal matrices: c[149 + j] = normal row j & 31 (3 floats)
// Vertex inputs: 0 position (3 floats), 6 matrix indices (position, texture 0-2), 7 (texture 3-6),
// 1 (texture 7), 2 normal (3 floats), 3 and 4 colors (bytes, 0-1), 8 + n texture coordinate n (2 floats).
//
// The matrix indices are the GameCube's own (XF row numbers: a vertex's position matrix is rows i .. i + 2,
// its normal matrix normal rows i & 31 ..), in the form the device's start-up test found to work
// (Device::vertexIndexType): bytes read as 0-255, bytes read as 0-1 (scaled back in the program), or floats.
// So the vertices a draw unpacks do not depend on the matrices' values, and unpacked display lists can be
// kept from frame to frame (the renderer's vertex cache); the renderer keeps the matrix memory's copy in
// the constants up to date, in order with the draws. Without relative addressing (index type 0) there are
// no vertex programs.
//
// Not supported (the renderer transforms such draws on the CPU): emboss (bump) texture coordinates and
// texture coordinates generated from binormals or tangents.
//
// Portable C++ (also built into the host tools).

#ifndef PS3_RSX_XF_PROGRAM_H
#define PS3_RSX_XF_PROGRAM_H

#include "../gpu/gpu_xf.h"
#include "device.h"
#include "vp_builder.h"

namespace gfx {

enum {
	XC_MISC0        = 0,
	XC_MISC1        = 1,
	XC_PROJ         = 2,
	XC_CHAN         = 6,
	XC_LIGHTS       = 10,
	XC_LIGHT_ROWS   = 6,
	XC_POST         = 58,
	XC_MISC2        = 82,
	XC_SETUP_COUNT  = 83, // constants c[0 .. XC_SETUP_COUNT - 1] come from the XF set-up
	XC_MIRROR       = 83, // XF matrix rows 0-63 (and two zero rows: indices up to 63, rows i .. i + 2)
	XC_NMIRROR      = 149, // normal rows (j & 31) for j = 0-65
	XC_MIRROR_ROWS  = 66,
};

enum { XA_POS = 0, XA_IDX_C = 1, XA_NRM = 2, XA_CLR0 = 3, XA_CLR1 = 4, XA_IDX_A = 6, XA_IDX_B = 7, XA_TEX0 = 8 };

struct XfKey {
	enum { BYTES = 96 };
	union {
		u8 b[BYTES];
		u32 w[BYTES / 4];
	};
	bool operator==(const XfKey& o) const;
	u32 hash() const;
};

// What a program needs from the vertices and the constants (worked out with the key).
struct XfInfo {
	VertexFormat format;   // the vertices the program reads (offsets in the gathered vertex)
	gpu::GatherSpec gather; // the same for CommandProcessor::gatherVertices (posMap/texMap left NULL)
	bool normals;          // the program reads the normal matrices (lighting)
	u32 texCount;          // texture coordinates the program outputs
	u32 texMatrices;       // bit per texture coordinate that uses a texture matrix (XF matrix rows)
	u32 texRows3;          // bit per texture coordinate whose matrix has 3 rows (else 2)
	u32 indexType;         // how the vertices pick matrices (Device::vertexIndexType)
};

// The key for an XF set-up (setup: xfPrepare'd and xfLimit'ed for the draw), the draw's vertex layout, the
// texture coordinates the fragment program reads and the matrix index type (Device::vertexIndexType, not 0).
// False: not supported (transform on the CPU).
bool makeXfKey(const gpu::XfSetup& setup, const gpu::VertexLayout& layout, u32 texCoords, u32 indexType, XfKey& key);
bool buildXfProgram(const XfKey& key, VertexProgram& program, XfInfo& info, const char** error);
// Constants c[0 .. XC_SETUP_COUNT - 1] for a set-up.
void xfSetupConstants(const gpu::XfSetup& setup, float out[XC_SETUP_COUNT][4]);

} // namespace gfx

#endif
