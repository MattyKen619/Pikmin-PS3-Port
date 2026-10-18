// PS3 port: the GameCube's transform unit in software (see gpu_xf.h).

#include "gpu_xf.h"

namespace gpu {

static inline u32 bits(u32 v, u32 shift, u32 count) { return (v >> shift) & ((1u << count) - 1); }
static inline float asFloat(u32 w)
{
	union {
		u32 u;
		float f;
	} x;
	x.u = w;
	return x.f;
}
// a >= 0 ? b : c, without a branch on the PS3's processor (a mispredicted branch costs it ~20 cycles).
// (NaN in a selects c, the same on every platform.)
static inline float selectF(float a, float b, float c)
{
#if defined(__powerpc__) || defined(__powerpc64__)
	float r;
	__asm__("fsel %0,%1,%2,%3" : "=f"(r) : "f"(a), "f"(b), "f"(c));
	return r;
#else
	return a >= 0 ? b : c;
#endif
}
static inline float maxf(float a, float b) { return selectF(a - b, a, b); }

// Byte values 0-255 as 0-1. (A table: on the PS3's processor, converting an integer to floating point
// goes through memory and stalls; loading a float does not.)
static float sUnit[256];
static bool sUnitReady;
static inline float clamp01(float v) { return selectF(v, selectF(v - 1.0f, 1.0f, v), 0.0f); }
static inline float sqrtF(float v) { return __builtin_sqrtf(v); }

void xfPrepare(const u32* xf, const VertexLayout& L, XfSetup& s)
{
	if (!sUnitReady) {
		for (int i = 0; i < 256; i++) {
			sUnit[i] = i / 255.0f;
		}
		sUnitReady = true;
	}
	s.xf     = xf;
	s.layout = &L;
	for (int i = 0; i < 6; i++) {
		s.proj[i] = asFloat(xf[0x1020 + i]);
	}
	s.ortho      = xf[0x1026] != 0;
	s.numChans   = xf[0x1009] & 3;
	s.numTexGens = xf[0x103F] & 0xF;
	s.dual       = xf[0x1012] & 1;
	s.emboss     = false;
	s.needNormal = false;
	s.chanMask   = 3;
	s.texLimit   = 8;

	u32 lights = 0;
	for (int c = 0; c < 2; c++) {
		for (int part = 0; part < 2; part++) {
			XfSetup::Part& p = s.part[c][part];
			const u32 ctrl   = xf[(part ? 0x1010 : 0x100E) + c];
			const u32 matR = xf[0x100C + c], ambR = xf[0x100A + c];
			p.matFromVertex = bits(ctrl, 0, 1);
			p.ambFromVertex = bits(ctrl, 6, 1);
			p.lighting      = bits(ctrl, 1, 1);
			p.lightMask     = bits(ctrl, 2, 4) | (bits(ctrl, 11, 4) << 4);
			p.diffuse       = bits(ctrl, 7, 2);
			p.attn          = bits(ctrl, 9, 1);
			p.spot          = bits(ctrl, 10, 1);
			for (int k = 0; k < 4; k++) {
				p.mat[k] = sUnit[(matR >> (24 - k * 8)) & 0xFF];
				p.amb[k] = sUnit[(ambR >> (24 - k * 8)) & 0xFF];
			}
			if (p.lighting && (u32)c < s.numChans) {
				lights |= p.lightMask;
				s.needNormal = true;
			}
		}
	}
	for (int l = 0; l < 8; l++) {
		if (!(lights & (1u << l))) {
			continue;
		}
		const u32* lt      = xf + 0x600 + l * 16;
		XfSetup::Light& li = s.light[l];
		for (int k = 0; k < 4; k++) {
			li.color[k] = sUnit[(lt[3] >> (24 - k * 8)) & 0xFF];
		}
		for (int k = 0; k < 3; k++) {
			li.A[k]   = asFloat(lt[4 + k]);
			li.K[k]   = asFloat(lt[7 + k]);
			li.pos[k] = asFloat(lt[10 + k]);
			li.dir[k] = asFloat(lt[13 + k]);
		}
	}
	for (u32 t = 0; t < 8; t++) {
		XfSetup::TexGen& g = s.texgen[t];
		const u32 reg      = xf[0x1040 + t];
		g.stq              = bits(reg, 1, 1);
		g.abc1             = bits(reg, 2, 1);
		g.type             = bits(reg, 4, 3);
		g.row              = bits(reg, 7, 5);
		g.from             = bits(reg, 12, 3);
		g.post             = NULL;
		g.normalize        = false;
		if (t < s.numTexGens) {
			if (g.type == 1) {
				s.emboss = true;
			}
			if (g.row == 1 || g.row == 3 || g.row == 4) {
				s.needNormal = true;
			}
			if (s.dual && g.type == 0) {
				const u32 post = xf[0x1050 + t];
				g.normalize    = bits(post, 8, 1);
				g.post         = (const float*)(xf + 0x500 + bits(post, 0, 6) * 4);
			}
		}
	}
}

void xfLimit(XfSetup& s, u32 chanMask, u32 texCoords)
{
	// (texture coordinates generated from colors need those channels; emboss ones need the others)
	u32 limit = s.emboss && s.numTexGens > texCoords ? s.numTexGens : texCoords;
	for (u32 t = 0; t < limit && t < s.numTexGens; t++) {
		if (s.texgen[t].type == 2) chanMask |= 1;
		if (s.texgen[t].type == 3) chanMask |= 2;
	}
	s.chanMask = chanMask;
	s.texLimit = limit;
}

void xfTransform(const XfSetup& s, const Vertex& in, XfVertex& out)
{
	// (XF memory read as floats directly: matrices and light parameters are floats)
	const float* xf       = (const float*)s.xf;
	const VertexLayout& L = *s.layout;
	const u32 idx         = in.pnmtx & 63;
	const float* m        = xf + idx * 4;
	float pos[3]          = { in.pos[0], in.pos[1], L.posCount == 3 ? in.pos[2] : 0.0f };
	float view[3];
	for (int r = 0; r < 3; r++) {
		view[r] = m[r * 4] * pos[0] + m[r * 4 + 1] * pos[1] + m[r * 4 + 2] * pos[2] + m[r * 4 + 3];
	}

	// projection
	const float* p = s.proj;
	if (!s.ortho) {
		out.pos[0] = p[0] * view[0] + p[1] * view[2];
		out.pos[1] = p[2] * view[1] + p[3] * view[2];
		out.pos[2] = p[4] * view[2] + p[5];
		out.pos[3] = -view[2];
	} else {
		out.pos[0] = p[0] * view[0] + p[1];
		out.pos[1] = p[2] * view[1] + p[3];
		out.pos[2] = p[4] * view[2] + p[5];
		out.pos[3] = 1.0f;
	}

	// normal
	float nrm[3] = { 0, 0, 1 };
	if (s.needNormal && L.nrmMode != ATTR_NONE) {
		const float* n = xf + 0x400 + (idx & 31) * 3;
		for (int r = 0; r < 3; r++) {
			nrm[r] = n[r * 3] * in.nrm[0] + n[r * 3 + 1] * in.nrm[1] + n[r * 3 + 2] * in.nrm[2];
		}
		float len = sqrtF(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
		if (len > 0) {
			const float il = 1.0f / len;
			nrm[0] *= il, nrm[1] *= il, nrm[2] *= il;
		}
	}

	// colors and lighting. The light factors (attenuation x diffuse term) depend on the light and on the
	// part's lighting functions; parts usually share them, so each is worked out once per vertex.
	float factor[8];
	int factorMode[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	for (u32 c = 0; c < 2; c++) {
		if (c >= s.numChans || !(s.chanMask & (1u << c))) {
			out.color[c][0] = out.color[c][1] = out.color[c][2] = out.color[c][3] = 0;
			continue;
		}
		float vtx[4] = { 1, 1, 1, 1 };
		if (L.clrMode[c] != ATTR_NONE) {
			for (int k = 0; k < 4; k++) {
				vtx[k] = sUnit[in.clr[c][k]];
			}
		} else if (c == 1 && L.clrMode[0] != ATTR_NONE) {
			for (int k = 0; k < 4; k++) {
				vtx[k] = sUnit[in.clr[0][k]];
			}
		}
		for (int part = 0; part < 2; part++) {
			const XfSetup::Part& pt = s.part[c][part];
			const int k0 = part ? 3 : 0, k1 = part ? 4 : 3;
			float mat[4], amb[4];
			for (int k = k0; k < k1; k++) {
				mat[k] = pt.matFromVertex ? vtx[k] : pt.mat[k];
				amb[k] = pt.ambFromVertex ? vtx[k] : pt.amb[k];
			}
			if (!pt.lighting) {
				for (int k = k0; k < k1; k++) {
					out.color[c][k] = mat[k];
				}
				continue;
			}
			float acc[4];
			for (int k = k0; k < k1; k++) {
				acc[k] = amb[k];
			}
			const int mode = (pt.attn ? 1 : 0) | (pt.spot ? 2 : 0) | (int)(pt.diffuse << 2);
			for (int l = 0; l < 8; l++) {
				if (!(pt.lightMask & (1u << l))) {
					continue;
				}
				const XfSetup::Light& lt = s.light[l];
				if (factorMode[l] != mode) {
					float dir[3], attn = 1.0f;
					if (pt.attn && !pt.spot) { // specular
						float len = sqrtF(lt.pos[0] * lt.pos[0] + lt.pos[1] * lt.pos[1] + lt.pos[2] * lt.pos[2]);
						float il  = len > 0 ? 1.0f / len : 0;
						for (int k = 0; k < 3; k++) {
							dir[k] = lt.pos[k] * il;
						}
						float nd   = nrm[0] * dir[0] + nrm[1] * dir[1] + nrm[2] * dir[2];
						float a    = nd >= 0 ? maxf(0, nrm[0] * lt.dir[0] + nrm[1] * lt.dir[1] + nrm[2] * lt.dir[2]) : 0;
						float cosA = lt.A[0] + lt.A[1] * a + lt.A[2] * a * a;
						float disA = lt.K[0] + lt.K[1] * a + lt.K[2] * a * a;
						attn       = disA != 0 ? maxf(0, cosA) / disA : 0;
					} else {
						for (int k = 0; k < 3; k++) {
							dir[k] = lt.pos[k] - view[k];
						}
						float d2 = dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2];
						float d  = sqrtF(d2);
						float id = d > 0 ? 1.0f / d : 0;
						for (int k = 0; k < 3; k++) {
							dir[k] *= id;
						}
						if (pt.attn) {
							float a    = maxf(0, dir[0] * lt.dir[0] + dir[1] * lt.dir[1] + dir[2] * lt.dir[2]);
							float cosA = lt.A[0] + lt.A[1] * a + lt.A[2] * a * a;
							float disA = lt.K[0] + lt.K[1] * d + lt.K[2] * d2;
							attn       = disA != 0 ? maxf(0, cosA) / disA : 0;
						}
					}
					float dif = 1.0f;
					if (pt.diffuse) {
						dif = nrm[0] * dir[0] + nrm[1] * dir[1] + nrm[2] * dir[2];
						if (pt.diffuse == 2) {
							dif = maxf(0, dif);
						}
					}
					factor[l]     = attn * dif;
					factorMode[l] = mode;
				}
				for (int k = k0; k < k1; k++) {
					acc[k] += factor[l] * lt.color[k];
				}
			}
			for (int k = k0; k < k1; k++) {
				out.color[c][k] = mat[k] * clamp01(acc[k]);
			}
		}
	}

	// texture coordinates
	for (u32 t = 0; t < s.texLimit; t++) {
		out.tex[t][0] = out.tex[t][1] = 0;
		out.tex[t][2] = 1;
		if (t >= s.numTexGens) {
			continue;
		}
		const XfSetup::TexGen& g = s.texgen[t];
		float src[4]             = { 0, 0, 1, 1 };
		switch (g.row) {
		case 0: src[0] = pos[0], src[1] = pos[1], src[2] = pos[2]; break;
		case 1: src[0] = in.nrm[0], src[1] = in.nrm[1], src[2] = in.nrm[2]; break;
		case 3: src[0] = in.tangent[0], src[1] = in.tangent[1], src[2] = in.tangent[2]; break;
		case 4: src[0] = in.binormal[0], src[1] = in.binormal[1], src[2] = in.binormal[2]; break;
		default:
			if (g.row >= 5 && g.row <= 12) {
				src[0] = in.tex[g.row - 5][0], src[1] = in.tex[g.row - 5][1];
			}
			break;
		}
		if (!g.abc1) {
			src[2] = 1.0f;
		}
		float ss = 0, tt = 0, q = 1;
		if (g.type == 0) {
			const float* tm = xf + (in.texmtx[t] & 63) * 4;
			ss = tm[0] * src[0] + tm[1] * src[1] + tm[2] * src[2] + tm[3] * src[3];
			tt = tm[4] * src[0] + tm[5] * src[1] + tm[6] * src[2] + tm[7] * src[3];
			if (g.stq) {
				q = tm[8] * src[0] + tm[9] * src[1] + tm[10] * src[2] + tm[11] * src[3];
			}
		} else if (g.type == 1) { // emboss: the source coordinate (the bump offset is not applied)
			ss = out.tex[g.from][0], tt = out.tex[g.from][1], q = out.tex[g.from][2];
		} else { // color: s = red, t = green of the channel
			int c = g.type == 2 ? 0 : 1;
			ss = out.color[c][0], tt = out.color[c][1];
		}
		if (g.post) {
			float v[3] = { ss, tt, q };
			if (g.normalize) {
				float len = sqrtF(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
				if (len > 0) {
					const float il = 1.0f / len;
					v[0] *= il, v[1] *= il, v[2] *= il;
				}
			}
			const float* pm = g.post;
			ss = pm[0] * v[0] + pm[1] * v[1] + pm[2] * v[2] + pm[3];
			tt = pm[4] * v[0] + pm[5] * v[1] + pm[6] * v[2] + pm[7];
			q  = pm[8] * v[0] + pm[9] * v[1] + pm[10] * v[2] + pm[11];
		}
		out.tex[t][0] = ss, out.tex[t][1] = tt, out.tex[t][2] = q;
	}
}

} // namespace gpu
