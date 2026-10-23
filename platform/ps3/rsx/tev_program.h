// PS3 port: the GameCube's pixel pipeline (TEV color combiners and alpha test) as RSX fragment programs.
//
// Each distinct TEV configuration (stage count, combiner settings, texture and color inputs, constant
// selections, swap tables, alpha test functions) becomes one fragment program, generated as NV40 assembly
// text and assembled at run time (fp_assembler.h). Register and constant values that change from draw to
// draw are program parameters:
//   c[0]  PREV's initial value   c[1]-c[3] C0-C2      (TEV registers: signed values / 255)
//   c[4]-c[7] K0-K3 (constant colors / 255)          c[8]  alpha test references (0-255 in x, y)
//
// The arithmetic follows the hardware's formulas (d +/- lerp(a, b, c), bias, scale, clamp) in floating
// point (1.0 = 255); the 8-bit truncations of the inputs a, b and c are approximated by clamping to 0-1.
//
// Portable C++ (also built into the host tools).

#ifndef PS3_RSX_TEV_PROGRAM_H
#define PS3_RSX_TEV_PROGRAM_H

#include "fp_assembler.h"


namespace gfx {

enum { NUM_TEV_PARAMS = 9 };

struct TevKey {
	enum { WORDS = 51 };
	u32 w[WORDS];
	bool operator==(const TevKey& o) const
	{
		for (int i = 0; i < WORDS; i++) {
			if (w[i] != o.w[i]) {
				return false;
			}
		}
		return true;
	}
	// (adds and rotations in two lanes, a multiply only at the end: the PPU multiplies slowly, and a chain of
	// 51 waited for each other)
	u32 hash() const
	{
		u32 a = 0x9E3779B9u, b = 0x85EBCA6Bu;
		for (int i = 0; i + 1 < WORDS; i += 2) {
			a = ((a + w[i]) << 7 | (a + w[i]) >> 25) ^ w[i];
			b = ((b + w[i + 1]) << 11 | (b + w[i + 1]) >> 21) ^ w[i + 1];
		}
		a += w[WORDS - 1];
		const u32 h = (a ^ (b << 13 | b >> 19)) * 0x2C1B3C6Du;
		return h ^ (h >> 15);
	}
};

// The key for the current BP registers (bp: the 256 BP registers).
void makeTevKey(const u32* bp, TevKey& key);

struct TevProgram {
	FragmentProgram fp;
	u32 params;     // parameters c[0..8] the program reads (bit per index)
	u32 texMaps;    // texture maps sampled (bit per map)
	u32 texCoords;  // texture coordinates read (bit per coordinate)
	u32 colorChans; // rasterized color channels read (bit per channel)
	bool alwaysKill; // the alpha test can never pass
	Str text;
};

bool buildTevProgram(const TevKey& key, TevProgram& out, const char** error);

// Fixed programs for the renderer's own drawing (texture unit 0, coordinate TEX0).
enum CopyMode {
	COPY_RGBA,      // as is
	COPY_RGB,       // alpha forced to 1
	COPY_R_ALL,     // red to all channels (R4, R8 and their intensity forms)
	COPY_RA,        // red to RGB, alpha kept (RA4, RA8)
	COPY_A_ALL,     // alpha to all channels (A8)
	COPY_G_ALL,     // green to all channels (G8)
	COPY_B_ALL,     // blue to all channels (B8)
	COPY_MODE_COUNT,
};
// intensity: RGB converted to the luma (Y) value first.
Str copyProgramText(CopyMode mode, bool intensity);

} // namespace gfx

#endif
