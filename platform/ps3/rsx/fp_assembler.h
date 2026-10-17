// PS3 port: assembles RSX fragment programs (NV40 "FP2.0" assembly text) into microcode at run time, with
// the assembler from PSL1GHT's cgcomp tool (fpasm/, MIT licence). The renderer generates one program per
// TEV configuration the game uses (tev_program.cpp), so they cannot be compiled ahead of time.
//
// Portable C++ (also built into the host tools, which run the programs in a software RSX).

#ifndef PS3_RSX_FP_ASSEMBLER_H
#define PS3_RSX_FP_ASSEMBLER_H

#include <vector>

namespace gfx {

typedef unsigned int u32;
typedef unsigned short u16;

struct FragmentProgram {
	// The microcode as the RSX reads it: 16-byte slots (instructions, and the constants that follow the
	// instructions using them), 32-bit words with their 16-bit halves swapped (as cgcomp writes .fpo files).
	std::vector<u32> ucode;
	u32 numRegs;
	u32 control;    // FP_CONTROL bits (uses KIL, writes depth, ...)
	u16 texcoords;  // texture coordinates read
	u16 texcoord2D; // ... of which only s and t are needed
	u16 texcoord3D;
	// Where the program's parameters c[index] (declared with "#var") go: one entry per use.
	struct Reloc {
		u32 index;
		u32 slot;
	};
	std::vector<Reloc> relocs;
};

// Returns false (and an error message) if the program does not assemble.
bool assembleFragmentProgram(const char* text, FragmentProgram& out, const char** error);

// A constant value as stored in the microcode.
static inline u32 fpConstantWord(float value)
{
	union {
		float f;
		u32 u;
	} x;
	x.f = value;
	return (x.u >> 16) | (x.u << 16);
}

// The microcode word's value with its halves swapped back (for reading the microcode).
static inline u32 fpWord(u32 stored) { return (stored >> 16) | (stored << 16); }

} // namespace gfx

#endif
