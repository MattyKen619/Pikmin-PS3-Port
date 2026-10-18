// PS3 port: builds RSX (NV40) vertex programs instruction by instruction, straight into microcode.
//
// The encoding follows PSL1GHT's cgcomp vertex program compiler (tools/cgcomp/source/compilervp.cpp), which
// follows nouveau's nvfx_vertprog.c; field definitions in fpasm/nv40_vertprog.h. Relative constant
// addressing (c[A0.x + n]) is encoded as nouveau does it (cgcomp's parser does not support it).
//
// Hardware rules the builder enforces: one instruction reads at most one input register and one constant
// (any number of temporaries); scalar operations (RCP, RSQ, ...) read their operand's x component after
// the swizzle. Constant indices stay below 256 (the instruction field the headers document is 8 bits).
//
// Portable C++ (the host tools run the microcode in software, tools/ps3/gx_host/soft_device.cpp).

#ifndef PS3_RSX_VP_BUILDER_H
#define PS3_RSX_VP_BUILDER_H

#include "rsx_alloc.h"

namespace gfx {

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

// A vertex program ready for the device: 4 words per instruction.
struct VertexProgram {
	Vec<u32> ucode;
	u32 instructions;
	u32 inputMask;  // bit per input attribute read
	u32 outputMask; // the RSX's VP_RESULT_EN bits for the outputs written
	u32 numRegs;    // temporaries used
};

namespace vp {

enum RegType { R_NONE, R_TEMP, R_INPUT, R_CONST, R_OUTPUT, R_ADDR };
enum Output { O_HPOS = 0, O_COL0 = 1, O_COL1 = 2, O_BFC0 = 3, O_BFC1 = 4, O_FOGC = 5, O_PSZ = 6, O_TEX0 = 7 };
enum Mask { M_X = 8, M_Y = 4, M_Z = 2, M_W = 1, M_XY = 12, M_XYZ = 14, M_XYZW = 15 };
// Vertex inputs (attribute slots), as the RSX names them
enum Input { I_POS = 0, I_WEIGHT = 1, I_NORMAL = 2, I_COL0 = 3, I_COL1 = 4, I_FOG = 5, I_6 = 6, I_7 = 7, I_TEX0 = 8 };

struct Src {
	u8 type;
	u16 index;
	u8 swz[4];  // 0-3 = x-w
	bool neg, abs;
	bool relative; // constant index + A0.<addrComp>
	u8 addrComp;

	// "xyzw"-style swizzle of this source (applied on top of its current one)
	Src operator()(const char* s) const;
	Src operator-() const
	{
		Src r = *this;
		r.neg = !r.neg;
		return r;
	}
	Src absolute() const
	{
		Src r = *this;
		r.abs = true;
		return r;
	}
};

struct Dst {
	u8 type;
	u8 index;
	u8 mask; // M_*
};

Src src(u8 type, u16 index);
inline Src T(int i) { return src(R_TEMP, (u16)i); }
inline Src V(int i) { return src(R_INPUT, (u16)i); }
inline Src C(int i) { return src(R_CONST, (u16)i); }
// c[A0.<comp> + offset]
Src CRel(int offset, int addrComp = 0);
inline Dst DT(int i, int mask = M_XYZW)
{
	Dst d = { R_TEMP, (u8)i, (u8)mask };
	return d;
}
inline Dst DO(int o, int mask = M_XYZW)
{
	Dst d = { R_OUTPUT, (u8)o, (u8)mask };
	return d;
}

class Builder {
public:
	Builder();

	// vector operations
	void mov(Dst d, Src a, bool sat = false) { vec(1, d, &a, NULL, NULL, sat); }
	void mul(Dst d, Src a, Src b, bool sat = false) { vec(2, d, &a, &b, NULL, sat); }
	void add(Dst d, Src a, Src b, bool sat = false) { vec(3, d, &a, NULL, &b, sat); }
	void mad(Dst d, Src a, Src b, Src c, bool sat = false) { vec(4, d, &a, &b, &c, sat); }
	void dp3(Dst d, Src a, Src b) { vec(5, d, &a, &b, NULL, false); }
	void dp4(Dst d, Src a, Src b) { vec(7, d, &a, &b, NULL, false); }
	void min(Dst d, Src a, Src b) { vec(9, d, &a, &b, NULL, false); }
	void max(Dst d, Src a, Src b) { vec(10, d, &a, &b, NULL, false); }
	void slt(Dst d, Src a, Src b) { vec(11, d, &a, &b, NULL, false); }
	void sge(Dst d, Src a, Src b) { vec(12, d, &a, &b, NULL, false); }
	void seq(Dst d, Src a, Src b) { vec(16, d, &a, &b, NULL, false); }
	void sne(Dst d, Src a, Src b) { vec(20, d, &a, &b, NULL, false); }
	// A0.x = floor(a.x)
	void arl(Src a);
	// scalar operations (a's x component, result in every component of the mask)
	void rcp(Dst d, Src a) { sca(2, d, a); }
	void rsq(Dst d, Src a) { sca(4, d, a); }

	// Ends the program. False (and *error) if an instruction broke a rule or there are too many.
	bool finish(VertexProgram& out, const char** error);
	u32 count() const { return (u32)(mCode.size() / 4); }

private:
	void vec(u32 op, Dst d, const Src* a, const Src* b, const Src* c, bool sat);
	void sca(u32 op, Dst d, Src a);
	u32* begin(const Src* s0, const Src* s1, const Src* s2);
	void dst(u32* hw, Dst d, bool scalarSlot);
	void source(u32* hw, const Src* s, int pos);

	Vec<u32> mCode;
	u32 mInputs, mOutputs, mTemps;
	const char* mError;
};

} // namespace vp
} // namespace gfx

#endif
