// PS3 port: XF set-ups as RSX vertex programs (see xf_program.h). The arithmetic mirrors xfTransform
// (platform/ps3/gpu/gpu_xf.cpp) step by step; the host tools check the two against each other.

#include "xf_program.h"

#include <math.h>
#include <string.h>

namespace gfx {

namespace {

// key layout (bytes)
enum {
	K_FLAGS   = 0,  // bit 0: normal attribute, bit 1: lighting needs transformed normals
	K_CLRSRC  = 1,  // 2 bytes: vertex color input per channel (0: none, white)
	K_TEXIN   = 3,  // texture coordinate inputs read
	K_CHANS   = 4,  // channels computed (bit per channel)
	K_PARTS   = 5,  // 4 parts x 7 bytes: lighting, material from vertex, ambient from vertex, light mask,
	                //   diffuse function, attenuation, spot
	K_TEXCNT  = 33, // texture coordinates output
	K_GENS    = 34, // 8 x 6 bytes: type (0xFF: none), row, stq, abc1, post, normalize
	K_INDEX   = 90, // matrix index type (Device::vertexIndexType)
	PART_SIZE = 7,
	GEN_SIZE  = 6,
};

} // namespace

bool XfKey::operator==(const XfKey& o) const
{
	for (int i = 0; i < BYTES / 4; i++) {
		if (w[i] != o.w[i]) {
			return false;
		}
	}
	return true;
}

u32 XfKey::hash() const
{
	u32 h = 2166136261u;
	for (int i = 0; i < BYTES / 4; i++) {
		h = (h ^ w[i]) * 16777619u;
	}
	return h;
}

bool makeXfKey(const gpu::XfSetup& s, const gpu::VertexLayout& L, u32 texCoords, u32 indexType, XfKey& key)
{
	if (indexType == 0) {
		return false; // (the matrices are picked with relative addressing)
	}
	memset(&key, 0, sizeof(key));
	key.b[K_INDEX] = (u8)indexType;
	u32 texCount = texCoords > s.texLimit ? texCoords : s.texLimit;
	if (texCount > 8) {
		texCount = 8;
	}
	bool rowNormal = false;
	for (u32 t = 0; t < texCount && t < s.numTexGens; t++) {
		const gpu::XfSetup::TexGen& g = s.texgen[t];
		if (g.type == 1) {
			return false; // emboss
		}
		if (g.type == 0 && (g.row == 3 || g.row == 4)) {
			return false; // binormal, tangent
		}
		if (g.type == 0 && g.row == 1) {
			rowNormal = true;
		}
	}
	const u32 chans = s.chanMask & ((1u << s.numChans) - 1) & 3;
	bool lighting   = false;
	for (u32 c = 0; c < 2; c++) {
		if (!(chans & (1u << c))) {
			continue;
		}
		bool vertexColor = false;
		for (u32 part = 0; part < 2; part++) {
			const gpu::XfSetup::Part& p = s.part[c][part];
			u8* k                        = key.b + K_PARTS + (c * 2 + part) * PART_SIZE;
			k[0]                         = p.lighting;
			k[1]                         = p.matFromVertex;
			k[2]                         = p.ambFromVertex && p.lighting;
			if (p.lighting) {
				k[3]     = (u8)p.lightMask;
				k[4]     = (u8)p.diffuse;
				k[5]     = p.attn;
				k[6]     = p.spot;
				lighting = true;
			}
			vertexColor |= k[1] || k[2];
		}
		if (vertexColor) {
			if (L.clrMode[c] != gpu::ATTR_NONE) {
				key.b[K_CLRSRC + c] = (u8)(XA_CLR0 + c);
			} else if (c == 1 && L.clrMode[0] != gpu::ATTR_NONE) {
				key.b[K_CLRSRC + c] = XA_CLR0;
			}
		}
	}
	key.b[K_CHANS] = (u8)chans;
	const bool needNormal = lighting || rowNormal;
	key.b[K_FLAGS]        = (u8)((needNormal && L.nrmMode != gpu::ATTR_NONE ? 1 : 0) | (lighting ? 2 : 0));
	key.b[K_TEXCNT]       = (u8)texCount;
	for (u32 t = 0; t < texCount; t++) {
		u8* k = key.b + K_GENS + t * GEN_SIZE;
		if (t >= s.numTexGens) {
			k[0] = 0xFF;
			continue;
		}
		const gpu::XfSetup::TexGen& g = s.texgen[t];
		k[0]                          = (u8)g.type;
		if (g.type == 0) {
			k[1] = (u8)g.row;
			k[2] = g.stq;
			k[3] = g.abc1;
			k[4] = g.post != NULL;
			k[5] = g.post != NULL && g.normalize;
			if (g.row >= 5 && g.row <= 12 && L.texMode[g.row - 5] != gpu::ATTR_NONE) {
				key.b[K_TEXIN] |= (u8)(1u << (g.row - 5));
			}
		} else {
			k[0] = g.type == 2 ? 2 : 3; // (color texture coordinates: channel 0 or 1)
		}
	}
	return true;
}

namespace {

using namespace vp;

// temporaries
enum {
	R_IDX_A = 0, R_IDX_B = 1, R_IDX_C = 2, // matrix indices (XF rows)
	R_VIEW = 3, R_MODEL = 4, R_NRM = 5,    // view and model space positions (w = 1), normal
	R_ACC = 6,                            // lighting sum
	R_CHAN0 = 7,                          // channel colors (7, 8)
	R_FACTOR0 = 9, R_FACTOR_END = 19,     // light factors kept for reuse
	R_FACTOR_SPARE = 19,                  // a factor when those are all taken (not kept)
	R_S0 = 20, R_S1 = 21, R_S2 = 22, R_S3 = 23, // light scratch
	R_TSRC = 28, R_TC = 29, R_TPOST = 30, // texture coordinate scratch
	R_NSCR = 31,
};

struct Gen {
	Builder b;
	const XfKey& k;
	int factorReg[8][16]; // per light and mode: the temporary holding its factor (0: not yet)
	int nextFactor;

	explicit Gen(const XfKey& key)
	    : k(key)
	    , nextFactor(R_FACTOR0)
	{
		memset(factorReg, 0, sizeof(factorReg));
	}

	// The light factor (attenuation x diffuse term) of light l for a part's functions, in some temporary's x.
	Src factor(int l, int diffuse, bool attn, bool spot)
	{
		const int mode = (attn ? 1 : 0) | (spot ? 2 : 0) | (diffuse << 2);
		if (factorReg[l][mode]) {
			return T(factorReg[l][mode])("x");
		}
		int rf = R_FACTOR_SPARE;
		if (nextFactor < R_FACTOR_END) {
			rf                 = nextFactor++;
			factorReg[l][mode] = rf;
		}
		const int L = XC_LIGHTS + l * XC_LIGHT_ROWS;
		const Src zero = C(XC_MISC0)("x"), one = C(XC_MISC0)("y");
		Src dir;
		if (attn && !spot) {
			// specular: the half-angle vector; attenuation by the normal's angle to the light's direction
			b.dp3(DT(R_S0, M_X), T(R_NRM), C(L + 5));          // n . half
			b.dp3(DT(R_S0, M_Y), T(R_NRM), C(L + 4));          // n . dir
			b.max(DT(R_S0, M_Y), T(R_S0)("y"), zero);
			b.sge(DT(R_S0, M_Z), T(R_S0)("x"), zero);          // facing the light
			b.mul(DT(R_S0, M_W), T(R_S0)("y"), T(R_S0)("z"));  // a
			b.mov(DT(R_S1, M_X), one);
			b.mov(DT(R_S1, M_Y), T(R_S0)("w"));
			b.mul(DT(R_S1, M_Z), T(R_S0)("w"), T(R_S0)("w"));  // (1, a, a^2)
			b.dp3(DT(R_S2, M_X), T(R_S1), C(L + 1));           // cos attenuation
			b.dp3(DT(R_S2, M_Y), T(R_S1), C(L + 2));           // distance attenuation
			dir = C(L + 5);
		} else {
			b.add(DT(R_S3, M_XYZ), C(L + 3), -T(R_VIEW));      // to the light
			b.dp3(DT(R_S0, M_X), T(R_S3), T(R_S3));            // d^2
			b.max(DT(R_S0, M_Y), T(R_S0)("x"), C(XC_MISC1)("x"));
			b.rsq(DT(R_S0, M_Y), T(R_S0)("y"));                // 1 / d
			b.mul(DT(R_S3, M_XYZ), T(R_S3), T(R_S0)("y"));     // direction
			b.mul(DT(R_S0, M_Z), T(R_S0)("x"), T(R_S0)("y"));  // d
			dir = T(R_S3);
			if (attn) {
				b.dp3(DT(R_S0, M_W), T(R_S3), C(L + 4));
				b.max(DT(R_S0, M_W), T(R_S0)("w"), zero);      // a
				b.mov(DT(R_S1, M_X), one);
				b.mov(DT(R_S1, M_Y), T(R_S0)("w"));
				b.mul(DT(R_S1, M_Z), T(R_S0)("w"), T(R_S0)("w"));
				b.dp3(DT(R_S2, M_X), T(R_S1), C(L + 1));       // A . (1, a, a^2)
				b.mov(DT(R_S1, M_Y), T(R_S0)("z"));
				b.mov(DT(R_S1, M_Z), T(R_S0)("x"));
				b.dp3(DT(R_S2, M_Y), T(R_S1), C(L + 2));       // K . (1, d, d^2)
			}
		}
		if (attn) {
			// max(0, cos) / dist, or 0 when dist is 0
			b.seq(DT(R_S2, M_Z), T(R_S2)("y"), zero);
			b.add(DT(R_S2, M_W), T(R_S2)("y"), T(R_S2)("z"));
			b.rcp(DT(R_S2, M_W), T(R_S2)("w"));
			b.max(DT(R_S2, M_X), T(R_S2)("x"), zero);
			b.mul(DT(R_S2, M_X), T(R_S2)("x"), T(R_S2)("w"));
			b.sne(DT(R_S2, M_Z), T(R_S2)("y"), zero);
			b.mul(DT(rf, M_X), T(R_S2)("x"), T(R_S2)("z"));
		} else {
			b.mov(DT(rf, M_X), one);
		}
		if (diffuse) {
			b.dp3(DT(R_S0, M_X), T(R_NRM), dir);
			if (diffuse == 2) {
				b.max(DT(R_S0, M_X), T(R_S0)("x"), zero);
			}
			b.mul(DT(rf, M_X), T(rf)("x"), T(R_S0)("x"));
		}
		return T(rf)("x");
	}
};

const char* const kComp[4] = { "x", "y", "z", "w" };

} // namespace

bool buildXfProgram(const XfKey& key, VertexProgram& program, XfInfo& info, const char** error)
{
	const u8* k          = key.b;
	const bool nrmAttr   = k[K_FLAGS] & 1;
	const bool lightNrm  = (k[K_FLAGS] & 2) != 0;
	const u32 texCount   = k[K_TEXCNT];
	const u32 texIn      = k[K_TEXIN];
	const u32 chans      = k[K_CHANS];

	// ---- the vertex layout
	memset(&info, 0, sizeof(info));
	VertexFormat& F    = info.format;
	gpu::GatherSpec& G = info.gather;
	G.nrm = G.clr[0] = G.clr[1] = gpu::GatherSpec::NO_OFFSET;
	for (int i = 0; i < 8; i++) {
		G.tex[i] = gpu::GatherSpec::NO_OFFSET;
	}
	u32 off = 0;
	F.attr[XA_POS].components = 3, F.attr[XA_POS].type = ATTRIB_F32, F.attr[XA_POS].offset = 0;
	G.pos = 0;
	off   = 12;
	const u32 indexType = k[K_INDEX];
	{
		const u32 group = indexType == ATTRIB_F32 ? 16 : 4; // (4 indices)
		G.index         = (u16)off;
		G.indexCount    = (u8)(1 + texCount);
		G.indexFloat    = indexType == ATTRIB_F32;
		F.attr[XA_IDX_A].components = 4, F.attr[XA_IDX_A].type = (u8)indexType, F.attr[XA_IDX_A].offset = (u16)off;
		off += group;
		if (texCount > 3) {
			F.attr[XA_IDX_B].components = 4, F.attr[XA_IDX_B].type = (u8)indexType, F.attr[XA_IDX_B].offset = (u16)off;
			off += group;
		}
		if (texCount > 7) {
			F.attr[XA_IDX_C].components = 4, F.attr[XA_IDX_C].type = (u8)indexType, F.attr[XA_IDX_C].offset = (u16)off;
			off += group;
		}
	}
	if (nrmAttr) {
		F.attr[XA_NRM].components = 3, F.attr[XA_NRM].type = ATTRIB_F32, F.attr[XA_NRM].offset = (u16)off;
		G.nrm = (u16)off;
		off += 12;
	}
	for (int c = 0; c < 2; c++) {
		const bool used = k[K_CLRSRC] == XA_CLR0 + c || k[K_CLRSRC + 1] == XA_CLR0 + c;
		if (used) {
			F.attr[XA_CLR0 + c].components = 4, F.attr[XA_CLR0 + c].type = ATTRIB_U8_NORM, F.attr[XA_CLR0 + c].offset = (u16)off;
			G.clr[c] = (u16)off;
			off += 4;
		}
	}
	for (int i = 0; i < 8; i++) {
		if (texIn & (1u << i)) {
			F.attr[XA_TEX0 + i].components = 2, F.attr[XA_TEX0 + i].type = ATTRIB_F32, F.attr[XA_TEX0 + i].offset = (u16)off;
			G.tex[i] = (u16)off;
			off += 8;
		}
	}
	F.stride      = off;
	G.stride      = off;
	info.normals   = lightNrm && nrmAttr;
	info.indexType = indexType;
	info.texCount  = texCount;
	for (u32 t = 0; t < texCount; t++) {
		if (k[K_GENS + t * GEN_SIZE] == 0) {
			info.texMatrices |= 1u << t;
			if (k[K_GENS + t * GEN_SIZE + 2]) {
				info.texRows3 |= 1u << t;
			}
		}
	}

	// ---- the program
	Gen g(key);
	Builder& b = g.b;
	const Src zero = C(XC_MISC0)("x"), one = C(XC_MISC0)("y");

	// matrix indices (bytes read as 0-1: * 255, and + 0.5 against rounding)
	{
		const int regs[3] = { R_IDX_A, R_IDX_B, R_IDX_C };
		const int ins[3]  = { XA_IDX_A, XA_IDX_B, XA_IDX_C };
		for (int i = 0; i < 3; i++) {
			if (i == 1 && texCount <= 3) break;
			if (i == 2 && texCount <= 7) break;
			if (indexType == ATTRIB_U8_NORM) {
				b.mad(DT(regs[i]), V(ins[i]), C(XC_MISC2)("x"), C(XC_MISC2)("y"));
			} else {
				b.mov(DT(regs[i]), V(ins[i]));
			}
		}
	}
	// position: model -> view -> clip (the position matrix: c[XC_MIRROR + index + row], A0 loaded with arl)
	b.mov(DT(R_MODEL, M_XYZ), V(XA_POS));
	b.mov(DT(R_MODEL, M_W), one);
	b.arl(T(R_IDX_A)("x"));
	b.dp4(DT(R_VIEW, M_X), CRel(XC_MIRROR + 0), T(R_MODEL));
	b.dp4(DT(R_VIEW, M_Y), CRel(XC_MIRROR + 1), T(R_MODEL));
	b.dp4(DT(R_VIEW, M_Z), CRel(XC_MIRROR + 2), T(R_MODEL));
	b.mov(DT(R_VIEW, M_W), one);
	static const int kMask[4] = { M_X, M_Y, M_Z, M_W };
	for (int r = 0; r < 4; r++) {
		b.dp4(DO(O_HPOS, kMask[r]), C(XC_PROJ + r), T(R_VIEW));
	}
	// normal (for lighting): the normal matrix, normalized; (0, 0, 1) without normals
	if (lightNrm) {
		if (nrmAttr) {
			b.dp3(DT(R_NRM, M_X), CRel(XC_NMIRROR + 0), V(XA_NRM)); // (A0: still the position index)
			b.dp3(DT(R_NRM, M_Y), CRel(XC_NMIRROR + 1), V(XA_NRM));
			b.dp3(DT(R_NRM, M_Z), CRel(XC_NMIRROR + 2), V(XA_NRM));
			b.dp3(DT(R_NSCR, M_X), T(R_NRM), T(R_NRM));
			b.max(DT(R_NSCR, M_X), T(R_NSCR)("x"), C(XC_MISC1)("x"));
			b.rsq(DT(R_NSCR, M_X), T(R_NSCR)("x"));
			b.mul(DT(R_NRM, M_XYZ), T(R_NRM), T(R_NSCR)("x"));
		} else {
			b.mov(DT(R_NRM), C(XC_MISC1)("zzwz"));
		}
	}

	// color channels
	for (int c = 0; c < 2; c++) {
		if (!(chans & (1u << c))) {
			continue;
		}
		const u8 srcSlot = k[K_CLRSRC + c];
		const Src vtx    = srcSlot ? V(srcSlot) : one;
		for (int part = 0; part < 2; part++) {
			const u8* p    = k + K_PARTS + (c * 2 + part) * PART_SIZE;
			const int mask = part ? M_W : M_XYZ;
			const Src mat  = p[1] ? vtx : C(XC_CHAN + c * 2);
			const Src amb  = p[2] ? vtx : C(XC_CHAN + c * 2 + 1);
			if (!p[0]) {
				b.mov(DT(R_CHAN0 + c, mask), mat);
				continue;
			}
			b.mov(DT(R_ACC, mask), amb);
			for (int l = 0; l < 8; l++) {
				if (p[3] & (1u << l)) {
					const Src f = g.factor(l, p[4], p[5] != 0, p[6] != 0);
					b.mad(DT(R_ACC, mask), f, C(XC_LIGHTS + l * XC_LIGHT_ROWS), T(R_ACC));
				}
			}
			b.mov(DT(R_ACC, mask), T(R_ACC), true); // clamp to 0-1
			b.mul(DT(R_CHAN0 + c, mask), mat, T(R_ACC));
		}
	}
	for (int c = 0; c < 2; c++) {
		b.mov(DO(O_COL0 + c), (chans & (1u << c)) ? T(R_CHAN0 + c) : zero);
	}

	// texture coordinates
	for (u32 t = 0; t < texCount; t++) {
		const u8* gk = k + K_GENS + t * GEN_SIZE;
		if (gk[0] == 0xFF) {
			b.mov(DO(O_TEX0 + t), C(XC_MISC1)("zzww")); // (0, 0, 1, 1)
			continue;
		}
		if (gk[0] == 0) {
			// the source row: (x, y, z or 1, 1)
			const u32 row = gk[1];
			if (row == 0) {
				b.mov(DT(R_TSRC), T(R_MODEL));
			} else if (row == 1) {
				if (nrmAttr) {
					b.mov(DT(R_TSRC, M_XYZ), V(XA_NRM));
					b.mov(DT(R_TSRC, M_W), one);
				} else {
					b.mov(DT(R_TSRC), C(XC_MISC0)("xxxy"));
				}
			} else if (row >= 5 && row <= 12 && (texIn & (1u << (row - 5)))) {
				b.mov(DT(R_TSRC, M_XY), V(XA_TEX0 + (row - 5)));
				b.mov(DT(R_TSRC, M_Z | M_W), one);
			} else {
				b.mov(DT(R_TSRC), C(XC_MISC1)("zzww"));
			}
			if ((row == 0 || row == 1) && !gk[3]) {
				b.mov(DT(R_TSRC, M_Z), one); // (AB11: z is 1)
			}
			// the texture matrix
			const int reg  = t < 3 ? R_IDX_A : t < 7 ? R_IDX_B : R_IDX_C;
			const int comp = t < 3 ? (int)t + 1 : t < 7 ? (int)t - 3 : 0;
			b.arl(T(reg)(kComp[comp]));
			b.dp4(DT(R_TC, M_X), CRel(XC_MIRROR + 0), T(R_TSRC));
			b.dp4(DT(R_TC, M_Y), CRel(XC_MIRROR + 1), T(R_TSRC));
			if (gk[2]) {
				b.dp4(DT(R_TC, M_Z), CRel(XC_MIRROR + 2), T(R_TSRC));
			} else {
				b.mov(DT(R_TC, M_Z), one);
			}
		} else {
			// from a color channel: s = red, t = green
			const int c = gk[0] == 2 ? 0 : 1;
			if (chans & (1u << c)) {
				b.mov(DT(R_TC, M_XY), T(R_CHAN0 + c));
			} else {
				b.mov(DT(R_TC, M_XY), zero);
			}
			b.mov(DT(R_TC, M_Z), one);
		}
		if (gk[0] == 0 && gk[4]) {
			// post-transform matrix, after normalizing if asked
			if (gk[5]) {
				b.dp3(DT(R_TPOST, M_X), T(R_TC), T(R_TC));
				b.max(DT(R_TPOST, M_X), T(R_TPOST)("x"), C(XC_MISC1)("x"));
				b.rsq(DT(R_TPOST, M_X), T(R_TPOST)("x"));
				b.mul(DT(R_TC, M_XYZ), T(R_TC), T(R_TPOST)("x"));
			}
			b.mov(DT(R_TC, M_W), one);
			const int P = XC_POST + (int)t * 3;
			b.dp4(DT(R_TPOST, M_X), C(P), T(R_TC));
			b.dp4(DT(R_TPOST, M_Y), C(P + 1), T(R_TC));
			b.dp4(DT(R_TPOST, M_Z), C(P + 2), T(R_TC));
			b.mov(DO(O_TEX0 + t), T(R_TPOST)("xyzz"));
		} else {
			b.mov(DO(O_TEX0 + t), T(R_TC)("xyzz")); // (s, t, q, q)
		}
	}
	return b.finish(program, error);
}

static void xfSetupProj(const gpu::XfSetup& s, float out[XC_SETUP_COUNT][4]);
static void xfSetupChan(const gpu::XfSetup& s, float out[XC_SETUP_COUNT][4]);
static void xfSetupLights(const gpu::XfSetup& s, float out[XC_SETUP_COUNT][4]);
static void xfSetupPost(const gpu::XfSetup& s, float out[XC_SETUP_COUNT][4]);

u32 xfSetupGroups(u32 address, u32 end)
{
	u32 g = 0;
	if (address < 0x600 && 0x500 < end) {
		g |= SG_POST; // post-transform matrices
	}
	if (address < 0x680 && 0x600 < end) {
		g |= SG_LIGHTS;
	}
	if (address < 0x1012 && 0x1009 < end) {
		g |= SG_CHAN | SG_LIGHTS; // channel count, colors, controls (which lights)
	}
	if (address < 0x1013 && 0x1012 < end) {
		g |= SG_POST; // dual texture transform
	}
	if (address < 0x1027 && 0x1020 < end) {
		g |= SG_PROJ;
	}
	if ((address < 0x1048 && 0x103F < end) || (address < 0x1058 && 0x1050 < end)) {
		g |= SG_POST; // texture coordinate generation
	}
	return g;
}

// The lights a set-up's programs read.
static u32 setupLights(const gpu::XfSetup& s)
{
	u32 lights = 0;
	for (int c = 0; c < 2; c++) {
		for (int part = 0; part < 2; part++) {
			if (s.part[c][part].lighting && (u32)c < s.numChans) {
				lights |= s.part[c][part].lightMask;
			}
		}
	}
	return lights;
}

// The texture coordinates whose post-transform matrix a set-up's programs read.
static u32 setupPosts(const gpu::XfSetup& s)
{
	u32 posts = 0;
	for (u32 t = 0; t < 8 && t < s.numTexGens; t++) {
		if (s.texgen[t].post && s.texgen[t].type == 0) {
			posts |= 1u << t;
		}
	}
	return posts;
}

u32 xfSetupRows(const gpu::XfSetup& s, u32 groups, u32 ranges[][2])
{
	u32 n = 0;
	if (groups & SG_PROJ) {
		ranges[n][0] = XC_MISC0, ranges[n][1] = XC_PROJ + 4, n++;
	}
	if (groups & SG_CHAN) {
		ranges[n][0] = XC_CHAN, ranges[n][1] = XC_CHAN + 4, n++;
	}
	if (groups & SG_LIGHTS) {
		const u32 lights = setupLights(s);
		for (u32 l = 0; l < 8; l++) {
			if (lights & (1u << l)) {
				const u32 first = XC_LIGHTS + l * XC_LIGHT_ROWS;
				if (n && ranges[n - 1][1] == first) {
					ranges[n - 1][1] = first + XC_LIGHT_ROWS;
				} else {
					ranges[n][0] = first, ranges[n][1] = first + XC_LIGHT_ROWS, n++;
				}
			}
		}
	}
	if (groups & SG_POST) {
		const u32 posts = setupPosts(s);
		for (u32 t = 0; t < 8; t++) {
			if (posts & (1u << t)) {
				const u32 first = XC_POST + t * 3;
				if (n && ranges[n - 1][1] == first) {
					ranges[n - 1][1] = first + 3;
				} else {
					ranges[n][0] = first, ranges[n][1] = first + 3, n++;
				}
			}
		}
	}
	if (groups & SG_PROJ) {
		ranges[n][0] = XC_MISC2, ranges[n][1] = XC_MISC2 + 1, n++;
	}
	return n;
}

void xfSetupConstants(const gpu::XfSetup& s, float out[XC_SETUP_COUNT][4], u32 groups)
{
	if (groups & SG_PROJ) {
		memset(out[XC_MISC0], 0, sizeof(float) * 4 * (XC_PROJ + 4 - XC_MISC0));
		memset(out[XC_MISC2], 0, sizeof(float) * 4);
		out[XC_MISC0][0] = 0, out[XC_MISC0][1] = 1, out[XC_MISC0][2] = 0.5f, out[XC_MISC0][3] = 2;
		out[XC_MISC1][0] = 1e-30f, out[XC_MISC1][1] = 0, out[XC_MISC1][2] = 0, out[XC_MISC1][3] = 1;
		out[XC_MISC2][0] = 255.0f, out[XC_MISC2][1] = 0.5f;
		xfSetupProj(s, out);
	}
	if (groups & SG_CHAN) {
		xfSetupChan(s, out);
	}
	if (groups & SG_LIGHTS) {
		xfSetupLights(s, out);
	}
	if (groups & SG_POST) {
		xfSetupPost(s, out);
	}
}

static void xfSetupProj(const gpu::XfSetup& s, float out[XC_SETUP_COUNT][4])
{
	const float* p = s.proj;
	float* r0 = out[XC_PROJ];
	float* r1 = out[XC_PROJ + 1];
	float* r2 = out[XC_PROJ + 2];
	float* r3 = out[XC_PROJ + 3];
	if (!s.ortho) {
		r0[0] = p[0], r0[2] = p[1];
		r1[1] = p[2], r1[2] = p[3];
		r2[2] = p[4], r2[3] = p[5];
		r3[2] = -1.0f;
	} else {
		r0[0] = p[0], r0[3] = p[1];
		r1[1] = p[2], r1[3] = p[3];
		r2[2] = p[4], r2[3] = p[5];
		r3[3] = 1.0f;
	}
}

static void xfSetupChan(const gpu::XfSetup& s, float out[XC_SETUP_COUNT][4])
{
	for (int c = 0; c < 2; c++) {
		for (int i = 0; i < 4; i++) {
			// (both parts hold the whole register; alpha in the alpha part's)
			out[XC_CHAN + c * 2][i]     = s.part[c][i == 3 ? 1 : 0].mat[i];
			out[XC_CHAN + c * 2 + 1][i] = s.part[c][i == 3 ? 1 : 0].amb[i];
		}
	}
}

static void xfSetupLights(const gpu::XfSetup& s, float out[XC_SETUP_COUNT][4])
{
	const u32 lights = setupLights(s);
	for (int l = 0; l < 8; l++) {
		if (!(lights & (1u << l))) {
			continue; // (xfPrepare reads only the lights in use; the programs too)
		}
		const gpu::XfSetup::Light& lt = s.light[l];
		float* L                      = out[XC_LIGHTS + l * XC_LIGHT_ROWS];
		memset(L, 0, sizeof(float) * 4 * XC_LIGHT_ROWS);
		for (int i = 0; i < 4; i++) {
			L[i] = lt.color[i];
		}
		for (int i = 0; i < 3; i++) {
			L[4 + i]  = lt.A[i];
			L[8 + i]  = lt.K[i];
			L[12 + i] = lt.pos[i];
			L[16 + i] = lt.dir[i];
		}
		L[15]     = 1.0f;
		float len = sqrtf(lt.pos[0] * lt.pos[0] + lt.pos[1] * lt.pos[1] + lt.pos[2] * lt.pos[2]);
		for (int i = 0; i < 3; i++) {
			L[20 + i] = len > 0 ? lt.pos[i] / len : 0.0f;
		}
	}
}

static void xfSetupPost(const gpu::XfSetup& s, float out[XC_SETUP_COUNT][4])
{
	const u32 posts = setupPosts(s);
	for (u32 t = 0; t < 8; t++) {
		if (posts & (1u << t)) {
			memcpy(out[XC_POST + t * 3], s.texgen[t].post, sizeof(float) * 12);
		}
	}
}

} // namespace gfx
