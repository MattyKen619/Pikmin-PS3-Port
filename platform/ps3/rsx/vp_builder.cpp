// PS3 port: NV40 vertex program microcode builder (see vp_builder.h).

#include "vp_builder.h"

#include "fpasm/nv40_vertprog.h"

namespace gfx {
namespace vp {

enum { MAX_INSTRUCTIONS = 512, MAX_CONST = 256, MAX_TEMPS = 32 };

Src src(u8 type, u16 index)
{
	Src s;
	s.type     = type;
	s.index    = index;
	s.swz[0]   = 0, s.swz[1] = 1, s.swz[2] = 2, s.swz[3] = 3;
	s.neg      = false;
	s.abs      = false;
	s.relative = false;
	s.addrComp = 0;
	return s;
}

Src CRel(int offset, int addrComp)
{
	Src s      = src(R_CONST, (u16)offset);
	s.relative = true;
	s.addrComp = (u8)(addrComp & 3);
	return s;
}

Src Src::operator()(const char* s) const
{
	Src r = *this;
	u8 picked[4];
	int n = 0;
	for (; s[n] && n < 4; n++) {
		int c   = s[n] == 'x' ? 0 : s[n] == 'y' ? 1 : s[n] == 'z' ? 2 : 3;
		picked[n] = swz[c];
	}
	for (int i = 0; i < 4; i++) {
		r.swz[i] = picked[i < n ? i : n - 1]; // ("x" means "xxxx", "xy" means "xyyy")
	}
	return r;
}

Builder::Builder()
    : mInputs(0)
    , mOutputs(0)
    , mTemps(0)
    , mError(NULL)
{
}

// A new instruction, after checking the operands against the one-input, one-constant rule.
u32* Builder::begin(const Src* s0, const Src* s1, const Src* s2)
{
	const Src* s[3] = { s0, s1, s2 };
	int input = -1, constant = -1, rel = -1;
	for (int i = 0; i < 3; i++) {
		if (!s[i]) {
			continue;
		}
		if (s[i]->type == R_INPUT) {
			if (input >= 0 && input != s[i]->index) {
				mError = "two different inputs in one instruction";
			}
			input = s[i]->index;
		} else if (s[i]->type == R_CONST) {
			int r = s[i]->relative ? 4 + s[i]->addrComp : 0;
			if (constant >= 0 && (constant != s[i]->index || rel != r)) {
				mError = "two different constants in one instruction";
			}
			if (s[i]->index >= MAX_CONST) {
				mError = "constant index out of range";
			}
			constant = s[i]->index;
			rel      = r;
		} else if (s[i]->type == R_TEMP && s[i]->index >= MAX_TEMPS) {
			mError = "temporary out of range";
		}
	}
	if (mCode.size() / 4 >= MAX_INSTRUCTIONS) {
		mError = "too many instructions";
	}
	size_t at = mCode.size();
	mCode.resize(at + 4, 0);
	u32* hw = &mCode[at];
	// condition: always true, identity swizzle
	hw[0] |= (NVFX_COND_TR << NV40_VP_INST_COND_SHIFT);
	hw[0] |= (0 << NV40_VP_INST_COND_SWZ_X_SHIFT) | (1 << NV40_VP_INST_COND_SWZ_Y_SHIFT) | (2 << NV40_VP_INST_COND_SWZ_Z_SHIFT)
	       | (3 << NV40_VP_INST_COND_SWZ_W_SHIFT);
	return hw;
}

void Builder::dst(u32* hw, Dst d, bool scalarSlot)
{
	switch (d.type) {
	case R_TEMP:
		if (d.index >= MAX_TEMPS) {
			mError = "temporary out of range";
		}
		if (d.index + 1u > mTemps) {
			mTemps = d.index + 1u;
		}
		// fall through
	case R_ADDR:
		hw[3] |= NV40_VP_INST_DEST_MASK;
		if (!scalarSlot) {
			hw[0] |= ((u32)d.index << NV40_VP_INST_VEC_DEST_TEMP_SHIFT);
		} else {
			hw[3] |= ((u32)d.index << NV40_VP_INST_SCA_DEST_TEMP_SHIFT);
		}
		break;
	case R_OUTPUT:
		switch (d.index) {
		case O_COL0:
		case O_COL1:
			mOutputs |= (1u << (d.index - O_COL0)) | (4u << (d.index - O_COL0));
			break;
		case O_BFC0:
		case O_BFC1:
			mOutputs |= (1u << (d.index - O_BFC0)) | (4u << (d.index - O_BFC0));
			break;
		case O_FOGC: mOutputs |= 1u << 4; break;
		case O_PSZ: mOutputs |= 1u << 5; break;
		default:
			if (d.index >= O_TEX0 && d.index < O_TEX0 + 8) {
				mOutputs |= 0x4000u << (d.index - O_TEX0);
			}
			break;
		}
		hw[3] |= ((u32)d.index << NV40_VP_INST_DEST_SHIFT);
		if (!scalarSlot) {
			hw[0] |= NV40_VP_INST_VEC_RESULT | NV40_VP_INST_VEC_DEST_TEMP_MASK;
		} else {
			hw[3] |= NV40_VP_INST_SCA_RESULT | NV40_VP_INST_SCA_DEST_TEMP_MASK;
		}
		break;
	default: // no destination
		hw[3] |= NV40_VP_INST_DEST_MASK;
		if (!scalarSlot) {
			hw[0] |= NV40_VP_INST_VEC_DEST_TEMP_MASK;
		} else {
			hw[3] |= NV40_VP_INST_SCA_DEST_TEMP_MASK;
		}
		break;
	}
}

void Builder::source(u32* hw, const Src* s, int pos)
{
	u32 sr     = 0;
	Src none   = src(R_NONE, 0);
	if (!s) {
		s = &none;
	}
	switch (s->type) {
	case R_TEMP:
		sr |= (NV40_VP_SRC_REG_TYPE_TEMP << NV40_VP_SRC_REG_TYPE_SHIFT);
		sr |= ((u32)s->index << NV40_VP_SRC_TEMP_SRC_SHIFT);
		break;
	case R_INPUT:
		sr |= (NV40_VP_SRC_REG_TYPE_INPUT << NV40_VP_SRC_REG_TYPE_SHIFT);
		mInputs |= 1u << s->index;
		hw[1] = (hw[1] & ~NV40_VP_INST_INPUT_SRC_MASK) | ((u32)s->index << NV40_VP_INST_INPUT_SRC_SHIFT);
		break;
	case R_CONST:
		sr |= (NV40_VP_SRC_REG_TYPE_CONST << NV40_VP_SRC_REG_TYPE_SHIFT);
		hw[1] = (hw[1] & ~NV40_VP_INST_CONST_SRC_MASK) | (((u32)s->index << NV40_VP_INST_CONST_SRC_SHIFT) & NV40_VP_INST_CONST_SRC_MASK);
		if (s->relative) {
			hw[3] |= NV40_VP_INST_INDEX_CONST;
			hw[0] |= ((u32)s->addrComp << NV40_VP_INST_ADDR_SWZ_SHIFT);
		}
		break;
	default:
		sr |= (NV40_VP_SRC_REG_TYPE_INPUT << NV40_VP_SRC_REG_TYPE_SHIFT);
		break;
	}
	if (s->neg) {
		sr |= NV40_VP_SRC_NEGATE;
	}
	if (s->abs) {
		hw[0] |= (1u << (21 + pos));
	}
	sr |= ((u32)s->swz[0] << NV40_VP_SRC_SWZ_X_SHIFT) | ((u32)s->swz[1] << NV40_VP_SRC_SWZ_Y_SHIFT)
	    | ((u32)s->swz[2] << NV40_VP_SRC_SWZ_Z_SHIFT) | ((u32)s->swz[3] << NV40_VP_SRC_SWZ_W_SHIFT);
	switch (pos) {
	case 0:
		hw[1] |= (((sr & NV40_VP_SRC0_HIGH_MASK) >> NV40_VP_SRC0_HIGH_SHIFT) << NV40_VP_INST_SRC0H_SHIFT);
		hw[2] |= ((sr & NV40_VP_SRC0_LOW_MASK) << NV40_VP_INST_SRC0L_SHIFT);
		break;
	case 1: hw[2] |= (sr << NV40_VP_INST_SRC1_SHIFT); break;
	default:
		hw[2] |= (((sr & NV40_VP_SRC2_HIGH_MASK) >> NV40_VP_SRC2_HIGH_SHIFT) << NV40_VP_INST_SRC2H_SHIFT);
		hw[3] |= ((sr & NV40_VP_SRC2_LOW_MASK) << NV40_VP_INST_SRC2L_SHIFT);
		break;
	}
}

void Builder::vec(u32 op, Dst d, const Src* a, const Src* b, const Src* c, bool sat)
{
	u32* hw = begin(a, b, c);
	if (mError) {
		return;
	}
	dst(hw, d, false);
	source(hw, a, 0);
	source(hw, b, 1);
	source(hw, c, 2);
	if (sat) {
		hw[0] |= NV40_VP_INST_SATURATE;
	}
	hw[1] |= (op << NV40_VP_INST_VEC_OPCODE_SHIFT);
	hw[3] |= NV40_VP_INST_SCA_DEST_TEMP_MASK;
	hw[3] |= ((u32)d.mask << NV40_VP_INST_VEC_WRITEMASK_SHIFT);
}

void Builder::sca(u32 op, Dst d, Src a)
{
	u32* hw = begin(NULL, NULL, &a);
	if (mError) {
		return;
	}
	dst(hw, d, true);
	source(hw, NULL, 0);
	source(hw, NULL, 1);
	source(hw, &a, 2);
	hw[1] |= (op << NV40_VP_INST_SCA_OPCODE_SHIFT);
	hw[0] |= NV40_VP_INST_VEC_DEST_TEMP_MASK;
	hw[3] |= ((u32)d.mask << NV40_VP_INST_SCA_WRITEMASK_SHIFT);
}

void Builder::arl(Src a)
{
	Dst d = { R_ADDR, 0, M_X };
	vec(13, d, &a, NULL, NULL, false);
}

bool Builder::finish(VertexProgram& out, const char** error)
{
	*error = NULL;
	if (mCode.empty() && !mError) {
		mov(DO(O_HPOS), V(0));
	}
	if (mError) {
		*error = mError;
		return false;
	}
	mCode[mCode.size() - 1] |= NVFX_VP_INST_LAST;
	out.ucode        = mCode;
	out.instructions = (u32)(mCode.size() / 4);
	out.inputMask    = mInputs;
	out.outputMask   = mOutputs;
	out.numRegs      = mTemps ? mTemps : 1;
	return true;
}

} // namespace vp
} // namespace gfx
