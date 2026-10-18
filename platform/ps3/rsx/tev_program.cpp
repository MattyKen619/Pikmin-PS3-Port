// PS3 port: TEV configurations as RSX fragment programs (see tev_program.h).
//
// Register use in the generated programs:
//   R0-R3  the TEV registers PREV, C0, C1, C2
//   R4     constants (0, 128/255, 1, 0.5): ZERO, HALF, ONE, and the factor for the 0.5 scale
//   R5     the stage's constant (KONST) color and alpha
//   R6     the stage's texture color (after the swap table)
//   R7     the stage's rasterized color (after the swap table)
//   R8     the stage's result: color in xyz, alpha in w
//   R9-R11 copies of inputs clamped to 0-1, and comparison work
//   R12    alpha test work
// Each instruction reads at most one constant (c[n]) - the hardware has one constant slot per instruction.

#include "tev_program.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace gfx {

static inline u32 bits(u32 v, u32 shift, u32 count) { return (v >> shift) & ((1u << count) - 1); }

void makeTevKey(const u32* bp, TevKey& k)
{
	memset(k.w, 0, sizeof(k.w));
	const u32 stages = bits(bp[0x00], 10, 4) + 1;
	k.w[0]           = stages;
	for (u32 s = 0; s < stages; s++) {
		k.w[1 + s * 2]     = bp[0xC0 + s * 2] & 0xFFFFFF;
		k.w[1 + s * 2 + 1] = bp[0xC1 + s * 2] & 0xFFFFFF;
	}
	for (u32 i = 0; i < 8; i++) {
		u32 tref = bp[0x28 + i] & 0xFFFFFF;
		u32 ksel = bp[0xF6 + i] & 0xFFFFFF;
		if (i * 2 >= stages) {
			tref = 0;
			ksel &= 0xF; // (the swap table bits stay: any stage may use any table)
		} else if (i * 2 + 1 >= stages) {
			tref &= 0xFFF;
			ksel &= 0x3FFF;
		}
		k.w[33 + i] = tref;
		k.w[41 + i] = ksel;
	}
	k.w[49] = bits(bp[0xF3], 16, 8); // alpha test functions and logic (the references are parameters)
	k.w[50] = 0;                     // (reserved: fog)
}

namespace {

class Asm {
public:
	Str text;
	void line(const char* fmt, ...)
	{
		char buf[256];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		text += buf;
		text += '\n';
	}
};

const char* const kSwz = "xyzw";

// konst fixed fractions (KCSEL/KASEL 0-7): 8/8 ... 1/8
const int kFixed[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };

enum { IMM_R4 = 16, IMM_MATH = 17, IMM_FIXED = 18 }; // immediate constants c[16], c[17], c[18..25]

struct Gen {
	Asm a;
	const TevKey& key;
	u32 params;
	u32 fixedUsed;
	u32 texMaps, texCoords, colorChans;
	bool regRead[4];

	explicit Gen(const TevKey& k)
	    : key(k)
	    , params(0)
	    , fixedUsed(0)
	    , texMaps(0)
	    , texCoords(0)
	    , colorChans(0)
	{
		regRead[0] = regRead[1] = regRead[2] = regRead[3] = false;
	}

	u32 stages() const { return key.w[0]; }
	u32 cenv(u32 s) const { return key.w[1 + s * 2]; }
	u32 aenv(u32 s) const { return key.w[2 + s * 2]; }
	u32 tref(u32 s) const { return bits(key.w[33 + s / 2], (s & 1) * 12, 12); }
	u32 ksel(u32 s) const { return key.w[41 + s / 2]; }
	u32 swapTable(u32 table, u32 channel) const // the source channel for channel (0-3) of a swap table
	{
		u32 word = key.w[41 + table * 2 + (channel >> 1)];
		return bits(word, (channel & 1) * 2, 2);
	}
	Str swizzle(u32 table) const
	{
		Str s(".");
		for (u32 c = 0; c < 4; c++) {
			s += kSwz[swapTable(table, c)];
		}
		return s;
	}

	// ---- operands. Color inputs (0-15) and alpha inputs (0-7) as source registers.
	static bool colorIsRegister(u32 in) { return in < 8; }
	static bool alphaIsRegister(u32 in) { return in < 4; }

	Str colorOperand(u32 in) const
	{
		static const char* const ops[16] = { "R0", "R0.wwww", "R1", "R1.wwww", "R2", "R2.wwww", "R3", "R3.wwww",
			                                 "R6", "R6.wwww", "R7", "R7.wwww", "R4.zzzz", "R4.yyyy", "R5", "R4.xxxx" };
		return ops[in & 15];
	}
	Str alphaOperand(u32 in) const
	{
		static const char* const ops[8] = { "R0", "R1", "R2", "R3", "R6", "R7", "R5", "R4.xxxx" };
		return ops[in & 7];
	}

	void noteColorInput(u32 in, bool& tex, bool& ras, bool& konst)
	{
		if (in < 8) {
			regRead[in / 2] = true;
		} else if (in == 8 || in == 9) {
			tex = true;
		} else if (in == 10 || in == 11) {
			ras = true;
		} else if (in == 14) {
			konst = true;
		}
	}
	void noteAlphaInput(u32 in, bool& tex, bool& ras, bool& konst)
	{
		if (in < 4) {
			regRead[in] = true;
		} else if (in == 4) {
			tex = true;
		} else if (in == 5) {
			ras = true;
		} else if (in == 6) {
			konst = true;
		}
	}

	// A constant's components for KCSEL/KASEL 16-31 (K0-K3 red, green, blue, alpha)
	Str konstLoad(u32 sel, bool alpha)
	{
		const char* mask = alpha ? ".w" : ".xyz";
		if (sel < 8) {
			fixedUsed |= 1u << sel;
			char buf[64];
			snprintf(buf, sizeof(buf), "MOVR R5%s, c[%d];", mask, IMM_FIXED + sel);
			return buf;
		}
		if (!alpha && sel >= 12 && sel < 16) {
			u32 reg = sel - 12;
			params |= 1u << (4 + reg);
			char buf[64];
			snprintf(buf, sizeof(buf), "MOVR R5.xyz, c[%u];", 4 + reg);
			return buf;
		}
		if (sel >= 16) {
			u32 comp = (sel - 16) / 4, reg = (sel - 16) % 4;
			params |= 1u << (4 + reg);
			char buf[64];
			snprintf(buf, sizeof(buf), "MOVR R5%s, c[%u].%c%c%c%c;", mask, 4 + reg, kSwz[comp], kSwz[comp], kSwz[comp], kSwz[comp]);
			return buf;
		}
		char buf[64];
		snprintf(buf, sizeof(buf), "MOVR R5%s, R4.xxxx;", mask);
		return buf;
	}

	// a, b and c take 8 bits of their input: register values (which may be out of range) are clamped.
	Str clampedOperand(const Str& op, bool isRegister, int scratch, const char* mask)
	{
		if (!isRegister) {
			return op;
		}
		a.line("MOVR_SAT R%d%s, %s;", scratch, mask, op.c_str());
		char buf[16];
		snprintf(buf, sizeof(buf), "R%d", scratch);
		return buf;
	}

	// One combiner (color: mask ".xyz", alpha: mask ".w") into R8.
	void combiner(u32 env, bool alpha)
	{
		const char* m = alpha ? ".w" : ".xyz";
		u32 ia, ib, ic, id;
		if (alpha) {
			ia = bits(env, 13, 3), ib = bits(env, 10, 3), ic = bits(env, 7, 3), id = bits(env, 4, 3);
		} else {
			ia = bits(env, 12, 4), ib = bits(env, 8, 4), ic = bits(env, 4, 4), id = bits(env, 0, 4);
		}
		const u32 bias = bits(env, 16, 2), sub = bits(env, 18, 1), scale = bits(env, 20, 2);
		const u32 zero = alpha ? 7 : 15;
		const u32 one  = alpha ? 99 : 12; // (alpha has no ONE input)
		Str A = alpha ? alphaOperand(ia) : colorOperand(ia);
		Str B = alpha ? alphaOperand(ib) : colorOperand(ib);
		Str C = alpha ? alphaOperand(ic) : colorOperand(ic);
		Str D = alpha ? alphaOperand(id) : colorOperand(id);
		const bool aReg = alpha ? alphaIsRegister(ia) : colorIsRegister(ia);
		const bool bReg = alpha ? alphaIsRegister(ib) : colorIsRegister(ib);
		const bool cReg = alpha ? alphaIsRegister(ic) : colorIsRegister(ic);

		if (bias == 3) {
			compare(env, alpha, m, A, B, C, D, aReg, bReg, cReg);
			return;
		}

		// lerp = a + c * (b - a)
		Str lerp; // operand holding it ("" = zero)
		if (ic == zero) {
			lerp = ia == zero ? "" : A;
		} else if (ic == one) {
			lerp = ib == zero ? "" : B;
		} else if (ia == zero && ib == zero) {
			lerp = "";
		} else {
			Str Cc = clampedOperand(C, cReg, 9, m);
			if (ia == zero) {
				Str Bc = clampedOperand(B, bReg, 10, m);
				a.line("MULR R8%s, %s, %s;", m, Bc.c_str(), Cc.c_str());
			} else if (ib == zero) {
				Str Ac = clampedOperand(A, aReg, 10, m);
				a.line("MADR R8%s, -%s, %s, %s;", m, Ac.c_str(), Cc.c_str(), Ac.c_str());
			} else {
				Str Ac = clampedOperand(A, aReg, 10, m);
				Str Bc = clampedOperand(B, bReg, 11, m);
				a.line("ADDR R8%s, %s, -%s;", m, Bc.c_str(), Ac.c_str());
				a.line("MADR R8%s, R8, %s, %s;", m, Cc.c_str(), Ac.c_str());
			}
			lerp = "R8";
		}
		if (!lerp.empty() && lerp != "R8") {
			// (a or b alone: still clamped)
			bool reg = lerp == A ? aReg : bReg;
			if (reg) {
				a.line("MOVR_SAT R8%s, %s;", m, lerp.c_str());
				lerp = "R8";
			}
		}
		// d +/- lerp
		if (lerp.empty()) {
			if (id == zero) {
				a.line("MOVR R8%s, R4.xxxx;", m);
			} else {
				a.line("MOVR R8%s, %s;", m, D.c_str());
			}
		} else if (id == zero) {
			if (sub) {
				a.line("MOVR R8%s, -%s;", m, lerp.c_str());
			} else if (lerp != "R8") {
				a.line("MOVR R8%s, %s;", m, lerp.c_str());
			}
		} else {
			a.line("ADDR R8%s, %s, %s%s;", m, D.c_str(), sub ? "-" : "", lerp.c_str());
		}
		if (bias == 1) {
			a.line("ADDR R8%s, R8, R4.yyyy;", m);
		} else if (bias == 2) {
			a.line("ADDR R8%s, R8, -R4.yyyy;", m);
		}
		if (scale == 1) {
			a.line("ADDR R8%s, R8, R8;", m);
		} else if (scale == 2) {
			a.line("ADDR R8%s, R8, R8;", m);
			a.line("ADDR R8%s, R8, R8;", m);
		} else if (scale == 3) {
			a.line("MULR R8%s, R8, R4.wwww;", m);
		}
	}

	// Comparison modes: d + (a > b or a == b ? c : 0), comparing one channel (R8), two (GR16), three
	// (BGR24) as one number, or each channel (RGB8 / A8). Values are compared as 8-bit integers.
	void compare(u32 env, bool alpha, const char* m, const Str& A, const Str& B, const Str& C,
	             const Str& D, bool aReg, bool bReg, bool cReg)
	{
		const u32 sub = bits(env, 18, 1), scale = bits(env, 20, 2);
		const u32 mode = scale; // 0 R8, 1 GR16, 2 BGR24, 3 RGB8 (alpha: A8)
		const bool eq  = sub;
		// round a and b to integers: R9 = floor(a * 255 + 0.5), R10 likewise
		Str Ac = clampedOperand(A, aReg, 9, alpha ? ".w" : ".xyzw");
		a.line("MADR R9, %s, c[%d].xxxx, R4.wwww;", Ac.c_str(), IMM_MATH);
		a.line("FLRR R9, R9;");
		Str Bc = clampedOperand(B, bReg, 10, alpha ? ".w" : ".xyzw");
		a.line("MADR R10, %s, c[%d].xxxx, R4.wwww;", Bc.c_str(), IMM_MATH);
		a.line("FLRR R10, R10;");
		const char* op = eq ? "SEQR" : "SGTR";
		Str sel; // the comparison result (0 or 1), as an operand
		if (alpha || mode == 3) {
			a.line("%s R9%s, R9, R10;", op, m);
			sel = "R9";
		} else {
			if (mode >= 1) { // pack: x + 256 y (+ 65536 z)
				if (mode == 2) {
					a.line("MADR R9.z, R9.zzzz, c[%d].yyyy, R9.yyyy;", IMM_MATH);
					a.line("MADR R9.x, R9.zzzz, c[%d].yyyy, R9.xxxx;", IMM_MATH);
					a.line("MADR R10.z, R10.zzzz, c[%d].yyyy, R10.yyyy;", IMM_MATH);
					a.line("MADR R10.x, R10.zzzz, c[%d].yyyy, R10.xxxx;", IMM_MATH);
				} else {
					a.line("MADR R9.x, R9.yyyy, c[%d].yyyy, R9.xxxx;", IMM_MATH);
					a.line("MADR R10.x, R10.yyyy, c[%d].yyyy, R10.xxxx;", IMM_MATH);
				}
			}
			a.line("%s R9.x, R9.xxxx, R10.xxxx;", op);
			sel = "R9.xxxx";
		}
		Str Cc = clampedOperand(C, cReg, 11, m);
		a.line("MADR R8%s, %s, %s, %s;", m, sel.c_str(), Cc.c_str(), D.c_str());
	}

	void stage(u32 s, bool last)
	{
		const u32 ce = cenv(s), ae = aenv(s), tr = tref(s);
		bool tex = false, ras = false, konstC = false, konstA = false;
		noteColorInput(bits(ce, 12, 4), tex, ras, konstC);
		noteColorInput(bits(ce, 8, 4), tex, ras, konstC);
		noteColorInput(bits(ce, 4, 4), tex, ras, konstC);
		noteColorInput(bits(ce, 0, 4), tex, ras, konstC);
		noteAlphaInput(bits(ae, 13, 3), tex, ras, konstA);
		noteAlphaInput(bits(ae, 10, 3), tex, ras, konstA);
		noteAlphaInput(bits(ae, 7, 3), tex, ras, konstA);
		noteAlphaInput(bits(ae, 4, 3), tex, ras, konstA);

		a.line("# stage %u", s);
		if (tex) {
			if (bits(tr, 6, 1)) {
				u32 map = bits(tr, 0, 3), coord = bits(tr, 3, 3);
				texMaps |= 1u << map;
				texCoords |= 1u << coord;
				a.line("TXPR R6, f[TEX%u], texture[%u], 2D;", coord, map);
				Str sw = swizzle(bits(ae, 2, 2));
				if (sw != ".xyzw") {
					a.line("MOVR R6, R6%s;", sw.c_str());
				}
			} else {
				a.line("MOVR R6, R4.zzzz;"); // (no texture: white)
			}
		}
		if (ras) {
			u32 chan = bits(tr, 7, 3);
			if (chan == 0 || chan == 1) {
				colorChans |= 1u << chan;
				a.line("MOVR R7, f[COL%u]%s;", chan, swizzle(bits(ae, 0, 2)).c_str());
			} else {
				a.line("MOVR R7, R4.xxxx;");
			}
		}
		const u32 ks = ksel(s);
		if (konstC) {
			a.line("%s", konstLoad(bits(ks, (s & 1) ? 14 : 4, 5), false).c_str());
		}
		if (konstA) {
			a.line("%s", konstLoad(bits(ks, (s & 1) ? 19 : 9, 5), true).c_str());
		}
		combiner(ce, false);
		combiner(ae, true);
		if (last) {
			return; // (the result stays in R8: the output)
		}
		const u32 cdest = bits(ce, 22, 2), adest = bits(ae, 22, 2);
		const bool cclamp = bits(ce, 19, 1), aclamp = bits(ae, 19, 1);
		a.line("MOVR%s R%u.xyz, R8;", cclamp ? "_SAT" : "", cdest);
		a.line("MOVR%s R%u.w, R8;", aclamp ? "_SAT" : "", adest);
	}

	// Alpha test: the comparisons' results as 0 or 1 for each possible outcome.
	static int compareConst(u32 func) { return func == 0 ? 0 : func == 7 ? 1 : -1; } // -1: depends on alpha
	static bool logic(u32 op, bool x, bool y) { return op == 0 ? (x && y) : op == 1 ? (x || y) : op == 2 ? (x != y) : (x == y); }

	void alphaTest(bool& alwaysKill)
	{
		const u32 at = key.w[49];
		const u32 f0 = bits(at, 0, 3), f1 = bits(at, 3, 3), op = bits(at, 6, 2);
		// outcomes over the possible comparison results
		bool canPass = false, canFail = false;
		for (int x = 0; x < 2; x++) {
			for (int y = 0; y < 2; y++) {
				int c0 = compareConst(f0), c1 = compareConst(f1);
				if ((c0 >= 0 && c0 != x) || (c1 >= 0 && c1 != y)) {
					continue;
				}
				if (logic(op, x != 0, y != 0)) {
					canPass = true;
				} else {
					canFail = true;
				}
			}
		}
		alwaysKill = !canPass;
		if (!canFail) {
			return;
		}
		a.line("# alpha test");
		if (!canPass) {
			a.line("MOVRC RC.x, R4.x;");
			a.line("KIL EQ.x;");
			return;
		}
		params |= 1u << 8;
		a.line("MADR R12.x, R8.wwww, c[%d].xxxx, R4.wwww;", IMM_MATH);
		a.line("FLRR R12.x, R12.xxxx;");
		static const char* const kSet[8] = { NULL, "SLTR", "SEQR", "SLER", "SGTR", "SNER", "SGER", NULL };
		for (int i = 0; i < 2; i++) {
			u32 f        = i ? f1 : f0;
			const char c = i ? 'z' : 'y';
			if (f == 0 || f == 7) {
				a.line("MOVR R12.%c, R4.%s;", c, f == 0 ? "xxxx" : "zzzz");
			} else {
				a.line("%s R12.%c, R12.xxxx, c[8].%s;", kSet[f], c, i ? "yyyy" : "xxxx");
			}
		}
		static const char* const kLogic[4] = { "MULR", "MAXR", "SNER", "SEQR" };
		a.line("%s R12.w, R12.yyyy, R12.zzzz;", kLogic[op]);
		a.line("MOVRC RC.x, R12.wwww;");
		a.line("KIL EQ.x;");
	}

	Str generate(bool& alwaysKill)
	{
		const u32 n = stages();
		for (u32 s = 0; s < n; s++) {
			stage(s, s + 1 == n);
		}
		a.line("MOVR_SAT R8, R8;");
		alphaTest(alwaysKill);
		a.line("MOVR o[COLR], R8;");
		a.line("END");
		Str stagesText;
		stagesText.swap(a.text);

		// header: parameters and immediate constants, then the register set-up
		a.line("!!FP2.0");
		// (registers read anywhere start with their values from before the draw - also covering components a
		// stage leaves alone when it writes only the color or only the alpha)
		for (u32 r = 0; r < 4; r++) {
			if (regRead[r]) {
				params |= 1u << r;
			}
		}
		for (u32 i = 0; i < NUM_TEV_PARAMS; i++) {
			if (params & (1u << i)) {
				a.line("#var float4 p%u :  : c[%u] : 1 : 1", i, i);
			}
		}
		a.line("#const c[%d] = 0 0.501960784 1 0.5", IMM_R4);
		a.line("#const c[%d] = 255 256 0 0", IMM_MATH);
		for (u32 k = 0; k < 8; k++) {
			if (fixedUsed & (1u << k)) {
				float v = kFixed[k] / 255.0f;
				a.line("#const c[%d] = %.9g %.9g %.9g %.9g", IMM_FIXED + k, v, v, v, v);
			}
		}
		a.line("MOVR R4, c[%d];", IMM_R4);
		for (u32 r = 0; r < 4; r++) {
			if (params & (1u << r)) {
				a.line("MOVR R%u, c[%u];", r, r);
			}
		}
		return a.text + stagesText;
	}
};

} // namespace

bool buildTevProgram(const TevKey& key, TevProgram& out, const char** error)
{
	Gen g(key);
	bool alwaysKill = false;
	out.text        = g.generate(alwaysKill);
	out.params      = g.params;
	out.texMaps     = g.texMaps;
	out.texCoords   = g.texCoords;
	out.colorChans  = g.colorChans;
	out.alwaysKill  = alwaysKill;
	return assembleFragmentProgram(out.text.c_str(), out.fp, error);
}

Str copyProgramText(CopyMode mode, bool intensity)
{
	Asm a;
	a.line("!!FP2.0");
	a.line("#const c[16] = 0.257 0.504 0.098 0.0627451");
	a.line("#const c[17] = 0 0 0 1");
	a.line("TEXR R0, f[TEX0], texture[0], 2D;");
	if (intensity && (mode == COPY_R_ALL || mode == COPY_RA)) {
		a.line("DP3R R1.x, R0, c[16];");
		a.line("ADDR_SAT R0.x, R1.xxxx, c[16].wwww;");
	}
	switch (mode) {
	case COPY_RGB: a.line("MOVR R0.w, c[17].wwww;"); break;
	case COPY_R_ALL: a.line("MOVR R0, R0.xxxx;"); break;
	case COPY_RA: a.line("MOVR R0.xyz, R0.xxxx;"); break;
	case COPY_A_ALL: a.line("MOVR R0, R0.wwww;"); break;
	case COPY_G_ALL: a.line("MOVR R0, R0.yyyy;"); break;
	case COPY_B_ALL: a.line("MOVR R0, R0.zzzz;"); break;
	default: break;
	}
	a.line("MOVR o[COLR], R0;");
	a.line("END");
	return a.text;
}

} // namespace gfx
