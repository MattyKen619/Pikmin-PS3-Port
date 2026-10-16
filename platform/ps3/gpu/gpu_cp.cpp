// PS3 port: the GameCube graphics processor's command interface, in software. See gpu_cp.h.

#include "gpu_cp.h"

#include <string.h>

// From the C library (the game's own headers, used by the PS3 build, don't declare them).
extern "C" void* realloc(void* ptr, size_t size);
extern "C" void free(void* ptr);

namespace gpu {

static inline u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static inline u32 bits(u32 v, u32 shift, u32 count) { return (v >> shift) & ((1u << count) - 1); }

static const u8 kCompSize[8] = { 1, 1, 2, 2, 4, 4, 4, 4 };
static const u8 kColorSize[8] = { 2, 3, 4, 2, 3, 4, 4, 4 };

CommandProcessor::CommandProcessor()
    : mMemory(NULL)
    , mListener(NULL)
    , mBpMask(0xFFFFFF)
    , mDisplayListDepth(0)
    , mPendingBuf(NULL)
    , mPending(0)
    , mPendingCap(0)
    , mVerts(NULL)
    , mVertCap(0)
{
	memset(&mState, 0, sizeof(mState));
	memset(&mStats, 0, sizeof(mStats));
}

CommandProcessor::~CommandProcessor()
{
	free(mPendingBuf);
	free(mVerts);
}

// ---------------------------------------------------------------- FIFO input

void CommandProcessor::push(const u8* bytes, u32 n)
{
	if (mPending == 0) {
		// Fast path: a whole command in this write (NOPs from GXFlush, for example).
		if (n == 1 && bytes[0] == OP_NOP) {
			mStats.commands++;
			return;
		}
	}
	if (mPending + n > mPendingCap) {
		u32 cap = mPendingCap ? mPendingCap * 2 : 4096;
		while (cap < mPending + n) {
			cap *= 2;
		}
		u8* buf = (u8*)realloc(mPendingBuf, cap);
		if (!buf) {
			return; // out of memory: drop (counted as a bad command later)
		}
		mPendingBuf = buf;
		mPendingCap = cap;
	}
	memcpy(mPendingBuf + mPending, bytes, n);
	mPending += n;

	// Commands are short except draws; don't re-parse a long draw for every byte that arrives.
	u8 op = mPendingBuf[0];
	if (op >= OP_DRAW_FIRST && op < 0xC0 && mPending >= 3) {
		VertexLayout layout;
		layoutFor(op & 7, layout);
		u32 need = 3 + be16(mPendingBuf + 1) * layout.size;
		if (mPending < need) {
			return;
		}
	}
	u32 used = run(mPendingBuf, mPending, false);
	if (used) {
		memmove(mPendingBuf, mPendingBuf + used, mPending - used);
		mPending -= used;
	}
}

void CommandProcessor::write(const u8* data, u32 size)
{
	if (mPending == 0) {
		// Usual case: whole commands; keep only an incomplete one at the end.
		u32 used = run(data, size, false);
		if (used < size) {
			push(data + used, size - used);
		}
		return;
	}
	push(data, size);
}

void CommandProcessor::write8(u32 value)
{
	u8 b = (u8)value;
	push(&b, 1);
}

void CommandProcessor::write16(u32 value)
{
	u8 b[2] = { (u8)(value >> 8), (u8)value };
	push(b, 2);
}

void CommandProcessor::write32(u32 value)
{
	u8 b[4] = { (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value };
	push(b, 4);
}

// ---------------------------------------------------------------- commands

u32 CommandProcessor::run(const u8* data, u32 size, bool inDisplayList)
{
	u32 pos = 0;
	while (pos < size) {
		const u8* p = data + pos;
		u32 avail   = size - pos;
		u8 op       = p[0];
		u32 used    = 0;
		switch (op) {
		case OP_NOP:
		case OP_INVAL_VCACHE:
			used = 1;
			break;
		case OP_LOAD_CP:
			if (avail >= 6) {
				loadCp(p[1], be32(p + 2));
				used = 6;
			}
			break;
		case OP_LOAD_XF:
			if (avail >= 5) {
				u32 header = be32(p + 1);
				u32 count  = ((header >> 16) & 0xF) + 1;
				if (avail >= 5 + count * 4) {
					loadXf(header & 0xFFFF, count, p + 5);
					used = 5 + count * 4;
				}
			}
			break;
		case OP_LOAD_INDX_A:
		case OP_LOAD_INDX_B:
		case OP_LOAD_INDX_C:
		case OP_LOAD_INDX_D:
			if (avail >= 5) {
				loadIndexed(op, be32(p + 1));
				used = 5;
			}
			break;
		case OP_CALL_DL:
			if (avail >= 9) {
				if (inDisplayList) {
					mStats.nestedDisplayLists++; // not supported by the hardware either
				} else {
					callDisplayList(be32(p + 1), be32(p + 5));
				}
				used = 9;
			}
			break;
		case OP_LOAD_BP:
			if (avail >= 5) {
				loadBp(be32(p + 1));
				used = 5;
			}
			break;
		default:
			if (op >= OP_DRAW_FIRST && op < 0xC0) {
				used = drawCommand(p, avail, inDisplayList);
			} else {
				mStats.badCommands++;
				if (mListener) {
					mListener->badCommand(op, p, avail, inDisplayList);
				}
				used = 1; // skip the byte (the hardware would hang)
			}
			break;
		}
		if (used == 0) {
			// Incomplete command.
			if (inDisplayList) {
				mStats.badCommands++;
				if (mListener) {
					mListener->badCommand(op, p, avail, true);
				}
				return size;
			}
			return pos;
		}
		mStats.commands++;
		pos += used;
	}
	return pos;
}

void CommandProcessor::loadCp(u32 reg, u32 value)
{
	mStats.cpLoads++;
	mState.cp[reg & 0xFF] = value;
}

void CommandProcessor::loadXf(u32 address, u32 count, const u8* values)
{
	mStats.xfLoads++;
	for (u32 i = 0; i < count; i++) {
		u32 a = address + i;
		if (a < sizeof(mState.xf) / sizeof(mState.xf[0])) {
			mState.xf[a] = be32(values + i * 4);
		} else {
			mStats.xfOutOfRange++;
		}
	}
}

void CommandProcessor::loadIndexed(u32 op, u32 word)
{
	mStats.indexedLoads++;
	u32 array  = ARRAY_INDX_A + ((op - OP_LOAD_INDX_A) >> 3);
	u32 index  = word >> 16;
	u32 count  = ((word >> 12) & 0xF) + 1;
	u32 xfAddr = word & 0xFFF;
	u32 base   = mState.cp[0xA0 + array];
	u32 stride = mState.cp[0xB0 + array];
	const u8* src = mMemory ? mMemory->read(base + index * stride, count * 4, MEM_ARRAY) : NULL;
	if (!src) {
		mStats.memoryFailures++;
		if (mListener) {
			mListener->memoryFailed(base + index * stride, count * 4, MEM_ARRAY);
		}
		return;
	}
	for (u32 i = 0; i < count; i++) {
		u32 a = xfAddr + i;
		if (a < sizeof(mState.xf) / sizeof(mState.xf[0])) {
			mState.xf[a] = be32(src + i * 4);
		}
	}
}

void CommandProcessor::loadBp(u32 word)
{
	mStats.bpLoads++;
	u32 reg   = word >> 24;
	u32 value = word & 0xFFFFFF;
	if (reg == 0xFE) {
		mBpMask = value; // applies to the next BP write only
		return;
	}
	value          = (mState.bp[reg] & ~mBpMask) | (value & mBpMask);
	mBpMask        = 0xFFFFFF;
	mState.bp[reg] = value;

	switch (reg) {
	case 0x45: // draw done
		if (value & 2) {
			mStats.drawDones++;
			if (mListener) {
				mListener->drawDone();
			}
		}
		break;
	case 0x47: // token
	case 0x48: // token with interrupt
		mStats.tokens++;
		if (mListener) {
			mListener->token((u16)value, reg == 0x48);
		}
		break;
	case 0x52: // EFB copy
	{
		CopyInfo c;
		c.srcLeft    = bits(mState.bp[0x49], 0, 10);
		c.srcTop     = bits(mState.bp[0x49], 10, 10);
		c.width      = bits(mState.bp[0x4A], 0, 10) + 1;
		c.height     = bits(mState.bp[0x4A], 10, 10) + 1;
		c.dest       = bits(mState.bp[0x4B], 0, 21) << 5;
		c.destStride = bits(mState.bp[0x4D], 0, 10) << 5;
		c.control    = value;
		c.toXfb      = (value >> 14) & 1;
		c.clear      = (value >> 11) & 1;
		if (c.toXfb) {
			mStats.copiesToXfb++;
		} else {
			mStats.copiesToTexture++;
		}
		if (mListener) {
			mListener->copy(c);
		}
		break;
	}
	case 0x65: // TLUT load: from the address in 0x64, count (x 16 entries) in bits 10-20
	{
		u32 address = bits(mState.bp[0x64], 0, 21) << 5;
		u32 bytes   = bits(value, 10, 11) * 32;
		if (bytes && mMemory && !mMemory->read(address, bytes, MEM_TLUT)) {
			mStats.memoryFailures++;
			if (mListener) {
				mListener->memoryFailed(address, bytes, MEM_TLUT);
			}
		}
		break;
	}
	default:
		break;
	}
	if (mListener) {
		mListener->bpWritten(reg, value);
	}
}

void CommandProcessor::callDisplayList(u32 address, u32 size)
{
	mStats.displayLists++;
	mStats.displayListBytes += size;
	const u8* list = mMemory ? mMemory->read(address, size, MEM_DISPLAY_LIST) : NULL;
	if (!list) {
		mStats.memoryFailures++;
		if (mListener) {
			mListener->memoryFailed(address, size, MEM_DISPLAY_LIST);
		}
		return;
	}
	mDisplayListDepth++;
	run(list, size, true);
	mDisplayListDepth--;
}

// ---------------------------------------------------------------- vertex formats

void CommandProcessor::layoutFor(u32 vat, VertexLayout& L) const
{
	memset(&L, 0, sizeof(L));
	vat &= 7;
	const u32 lo = mState.cp[0x50];
	const u32 hi = mState.cp[0x60];
	const u32 a  = mState.cp[0x70 + vat];
	const u32 b  = mState.cp[0x80 + vat];
	const u32 c  = mState.cp[0x90 + vat];

	L.pnmtx = bits(lo, 0, 1);
	for (int i = 0; i < 8; i++) {
		L.texmtx[i] = bits(lo, 1 + i, 1);
	}
	L.posMode     = bits(lo, 9, 2);
	L.nrmMode     = bits(lo, 11, 2);
	L.clrMode[0]  = bits(lo, 13, 2);
	L.clrMode[1]  = bits(lo, 15, 2);
	for (int i = 0; i < 8; i++) {
		L.texMode[i] = bits(hi, i * 2, 2);
	}

	L.posCount       = bits(a, 0, 1) ? 3 : 2;
	L.posType        = bits(a, 1, 3);
	L.posFrac        = bits(a, 4, 5);
	L.nrmNbt         = bits(a, 9, 1);
	L.nrmType        = bits(a, 10, 3);
	L.clrHasAlpha[0] = bits(a, 13, 1);
	L.clrFormat[0]   = bits(a, 14, 3);
	L.clrHasAlpha[1] = bits(a, 17, 1);
	L.clrFormat[1]   = bits(a, 18, 3);
	L.byteDequant    = bits(a, 30, 1);
	L.nrmIndex3      = bits(a, 31, 1);

	static const u8 kCntShift[8]  = { 21, 0, 9, 18, 27, 5, 14, 23 };
	static const u8 kFmtShift[8]  = { 22, 1, 10, 19, 28, 6, 15, 24 };
	static const u8 kFracShift[8] = { 25, 4, 13, 22, 0, 9, 18, 27 };
	static const u8 kCntReg[8]    = { 0, 1, 1, 1, 1, 2, 2, 2 }; // 0 = A, 1 = B, 2 = C
	static const u8 kFracReg[8]   = { 0, 1, 1, 1, 2, 2, 2, 2 };
	const u32 regs[3] = { a, b, c };
	for (int i = 0; i < 8; i++) {
		L.texCount[i] = bits(regs[kCntReg[i]], kCntShift[i], 1) ? 2 : 1;
		L.texType[i]  = bits(regs[kCntReg[i]], kFmtShift[i], 3);
		L.texFrac[i]  = bits(regs[kFracReg[i]], kFracShift[i], 5);
	}

	// Size in the command stream
	u32 size = L.pnmtx;
	for (int i = 0; i < 8; i++) {
		size += L.texmtx[i];
	}
	static const u8 kIndexSize[4] = { 0, 0, 1, 2 };
	if (L.posMode == ATTR_DIRECT) {
		size += L.posCount * kCompSize[L.posType];
	} else {
		size += kIndexSize[L.posMode];
	}
	if (L.nrmMode == ATTR_DIRECT) {
		size += (L.nrmNbt ? 9 : 3) * kCompSize[L.nrmType];
	} else if (L.nrmMode != ATTR_NONE) {
		size += kIndexSize[L.nrmMode] * ((L.nrmNbt && L.nrmIndex3) ? 3 : 1);
	}
	for (int i = 0; i < 2; i++) {
		if (L.clrMode[i] == ATTR_DIRECT) {
			size += kColorSize[L.clrFormat[i]];
		} else {
			size += kIndexSize[L.clrMode[i]];
		}
	}
	for (int i = 0; i < 8; i++) {
		if (L.texMode[i] == ATTR_DIRECT) {
			size += L.texCount[i] * kCompSize[L.texType[i]];
		} else {
			size += kIndexSize[L.texMode[i]];
		}
	}
	L.size = size;
}

// ---------------------------------------------------------------- vertex decoding

static inline float readComp(const u8* p, u32 type, float scale)
{
	switch (type) {
	case COMP_U8: return p[0] * scale;
	case COMP_S8: return (s8)p[0] * scale;
	case COMP_U16: return be16(p) * scale;
	case COMP_S16: return (s16)be16(p) * scale;
	default:
	{
		union {
			u32 u;
			float f;
		} v;
		v.u = be32(p);
		return v.f;
	}
	}
}

static void readColor(const u8* p, u32 format, u8* out)
{
	switch (format) {
	case CLR_RGB565:
	{
		u32 v  = be16(p);
		u32 r  = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
		out[0] = (u8)((r << 3) | (r >> 2));
		out[1] = (u8)((g << 2) | (g >> 4));
		out[2] = (u8)((b << 3) | (b >> 2));
		out[3] = 255;
		break;
	}
	case CLR_RGB888:
	case CLR_RGB888X:
		out[0] = p[0];
		out[1] = p[1];
		out[2] = p[2];
		out[3] = 255;
		break;
	case CLR_RGBA4444:
	{
		u32 v  = be16(p);
		out[0] = (u8)(((v >> 12) & 15) * 17);
		out[1] = (u8)(((v >> 8) & 15) * 17);
		out[2] = (u8)(((v >> 4) & 15) * 17);
		out[3] = (u8)((v & 15) * 17);
		break;
	}
	case CLR_RGBA6666:
	{
		u32 v = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
		for (int i = 0; i < 4; i++) {
			u32 x  = (v >> (18 - 6 * i)) & 63;
			out[i] = (u8)((x << 2) | (x >> 4));
		}
		break;
	}
	default: // RGBA8888
		out[0] = p[0];
		out[1] = p[1];
		out[2] = p[2];
		out[3] = p[3];
		break;
	}
}

namespace {

// One attribute's source for a draw: the stream, or an array (read once per draw).
struct ArraySource {
	const u8* data; // array memory from base + minIndex * stride
	u32 minIndex;
	u32 maxIndex;
	u32 stride;
	u32 elemSize;
	bool used;
};

} // namespace

void CommandProcessor::decodeVertices(const VertexLayout& L, const u8* data, u32 count)
{
	if (count > mVertCap) {
		u32 cap = mVertCap ? mVertCap : 256;
		while (cap < count) {
			cap *= 2;
		}
		Vertex* v = (Vertex*)realloc(mVerts, cap * sizeof(Vertex));
		if (!v) {
			mStats.vertexBufferOverflows++;
			return;
		}
		mVerts   = v;
		mVertCap = cap;
	}

	// Pass 1: offsets of each attribute in a stream vertex, and the index ranges of indexed attributes.
	static const u8 kIndexSize[4] = { 0, 0, 1, 2 };
	u32 off = L.pnmtx;
	u32 texMtxOff[8];
	for (int i = 0; i < 8; i++) {
		texMtxOff[i] = off;
		off += L.texmtx[i];
	}
	const u32 posOff = off;
	off += L.posMode == ATTR_DIRECT ? L.posCount * kCompSize[L.posType] : kIndexSize[L.posMode];
	const u32 nrmOff  = off;
	const u32 nrmIdxN = (L.nrmNbt && L.nrmIndex3) ? 3 : 1;
	if (L.nrmMode == ATTR_DIRECT) {
		off += (L.nrmNbt ? 9 : 3) * kCompSize[L.nrmType];
	} else {
		off += kIndexSize[L.nrmMode] * nrmIdxN;
	}
	u32 clrOff[2];
	for (int i = 0; i < 2; i++) {
		clrOff[i] = off;
		off += L.clrMode[i] == ATTR_DIRECT ? kColorSize[L.clrFormat[i]] : kIndexSize[L.clrMode[i]];
	}
	u32 texOff[8];
	for (int i = 0; i < 8; i++) {
		texOff[i] = off;
		off += L.texMode[i] == ATTR_DIRECT ? L.texCount[i] * kCompSize[L.texType[i]] : kIndexSize[L.texMode[i]];
	}

	ArraySource src[12];
	memset(src, 0, sizeof(src));
	const u32 nrmElem = (L.nrmNbt && !L.nrmIndex3) ? 9 : 3;
	struct Attr {
		u32 array, mode, offset, elemSize, indices;
	} attrs[12];
	int nattrs = 0;
	if (L.posMode >= ATTR_INDEX8) {
		attrs[nattrs++] = { ARRAY_POS, L.posMode, posOff, (u32)L.posCount * kCompSize[L.posType], 1 };
	}
	if (L.nrmMode >= ATTR_INDEX8) {
		attrs[nattrs++] = { ARRAY_NRM, L.nrmMode, nrmOff, nrmElem * kCompSize[L.nrmType], nrmIdxN };
	}
	for (int i = 0; i < 2; i++) {
		if (L.clrMode[i] >= ATTR_INDEX8) {
			attrs[nattrs++] = { (u32)(ARRAY_CLR0 + i), L.clrMode[i], clrOff[i], kColorSize[L.clrFormat[i]], 1 };
		}
	}
	for (int i = 0; i < 8; i++) {
		if (L.texMode[i] >= ATTR_INDEX8) {
			attrs[nattrs++] = { (u32)(ARRAY_TEX0 + i), L.texMode[i], texOff[i], (u32)L.texCount[i] * kCompSize[L.texType[i]], 1 };
		}
	}
	for (int k = 0; k < nattrs; k++) {
		const Attr& A   = attrs[k];
		ArraySource& S  = src[A.array];
		u32 lo = 0xFFFFFFFF, hi = 0;
		const u32 isz   = kIndexSize[A.mode];
		for (u32 v = 0; v < count; v++) {
			const u8* p = data + v * L.size + A.offset;
			for (u32 n = 0; n < A.indices; n++) {
				u32 idx = isz == 1 ? p[n] : be16(p + n * 2);
				if (A.array == ARRAY_POS && idx == (isz == 1 ? 0xFFu : 0xFFFFu)) {
					continue; // "skip this vertex" marker, not an array element
				}
				lo = idx < lo ? idx : lo;
				hi = idx > hi ? idx : hi;
			}
		}
		if (count == 0 || lo > hi) {
			continue;
		}
		S.stride   = mState.cp[0xB0 + A.array] & 0xFF;
		S.elemSize = A.elemSize;
		S.minIndex = lo;
		S.maxIndex = hi;
		u32 base   = mState.cp[0xA0 + A.array];
		u32 bytes  = (hi - lo) * S.stride + A.elemSize;
		S.data     = mMemory ? mMemory->read(base + lo * S.stride, bytes, MEM_ARRAY) : NULL;
		S.used     = true;
		if (!S.data) {
			mStats.memoryFailures++;
			if (mListener) {
				mListener->memoryFailed(base + lo * S.stride, bytes, MEM_ARRAY);
			}
		}
	}

	// Pass 2: decode.
	const u32 matA = mState.cp[0x30];
	const u32 matB = mState.cp[0x40];
	const float posScale = (L.posType == COMP_F32) ? 1.0f : 1.0f / (float)(1u << L.posFrac);
	const float nrmScale = (L.nrmType == COMP_S8 || L.nrmType == COMP_U8) ? 1.0f / 64.0f : 1.0f / 16384.0f;
	float texScale[8];
	for (int i = 0; i < 8; i++) {
		texScale[i] = (L.texType[i] == COMP_F32) ? 1.0f : 1.0f / (float)(1u << L.texFrac[i]);
	}

	for (u32 v = 0; v < count; v++) {
		const u8* p = data + v * L.size;
		Vertex& out = mVerts[v];
		memset(&out, 0, sizeof(out));

		out.pnmtx = L.pnmtx ? (p[0] & 63) : (u8)bits(matA, 0, 6);
		for (int i = 0; i < 8; i++) {
			u8 def        = (u8)(i < 4 ? bits(matA, 6 + i * 6, 6) : bits(matB, (i - 4) * 6, 6));
			out.texmtx[i] = L.texmtx[i] ? (p[texMtxOff[i]] & 63) : def;
		}

		// position
		if (L.posMode != ATTR_NONE) {
			const u8* q = NULL;
			if (L.posMode == ATTR_DIRECT) {
				q = p + posOff;
			} else {
				const ArraySource& S = src[ARRAY_POS];
				u32 idx = L.posMode == ATTR_INDEX8 ? p[posOff] : be16(p + posOff);
				q       = (S.data && idx >= S.minIndex && idx <= S.maxIndex) ? S.data + (idx - S.minIndex) * S.stride : NULL;
			}
			if (q) {
				const u32 cs = kCompSize[L.posType];
				for (u32 k = 0; k < L.posCount; k++) {
					out.pos[k] = readComp(q + k * cs, L.posType, posScale);
				}
			}
		}

		// normal (and binormal/tangent)
		if (L.nrmMode != ATTR_NONE) {
			const u32 cs = kCompSize[L.nrmType];
			float* dst[3] = { out.nrm, out.binormal, out.tangent };
			const u32 vectors = L.nrmNbt ? 3 : 1;
			for (u32 n = 0; n < vectors; n++) {
				const u8* q = NULL;
				if (L.nrmMode == ATTR_DIRECT) {
					q = p + nrmOff + n * 3 * cs;
				} else {
					const ArraySource& S = src[ARRAY_NRM];
					u32 which = (L.nrmNbt && L.nrmIndex3) ? n : 0;
					u32 isz   = kIndexSize[L.nrmMode];
					u32 idx   = isz == 1 ? p[nrmOff + which] : be16(p + nrmOff + which * 2);
					q         = (S.data && idx >= S.minIndex && idx <= S.maxIndex) ? S.data + (idx - S.minIndex) * S.stride : NULL;
					if (q && !(L.nrmNbt && L.nrmIndex3)) {
						q += n * 3 * cs; // one index for all three vectors
					}
				}
				if (q) {
					for (u32 k = 0; k < 3; k++) {
						dst[n][k] = readComp(q + k * cs, L.nrmType, nrmScale);
					}
				}
			}
		}

		// colors
		for (int i = 0; i < 2; i++) {
			if (L.clrMode[i] == ATTR_NONE) {
				continue;
			}
			const u8* q = NULL;
			if (L.clrMode[i] == ATTR_DIRECT) {
				q = p + clrOff[i];
			} else {
				const ArraySource& S = src[ARRAY_CLR0 + i];
				u32 idx = L.clrMode[i] == ATTR_INDEX8 ? p[clrOff[i]] : be16(p + clrOff[i]);
				q       = (S.data && idx >= S.minIndex && idx <= S.maxIndex) ? S.data + (idx - S.minIndex) * S.stride : NULL;
			}
			if (q) {
				readColor(q, L.clrFormat[i], out.clr[i]);
			}
		}

		// texture coordinates
		for (int i = 0; i < 8; i++) {
			if (L.texMode[i] == ATTR_NONE) {
				continue;
			}
			const u8* q = NULL;
			if (L.texMode[i] == ATTR_DIRECT) {
				q = p + texOff[i];
			} else {
				const ArraySource& S = src[ARRAY_TEX0 + i];
				u32 idx = L.texMode[i] == ATTR_INDEX8 ? p[texOff[i]] : be16(p + texOff[i]);
				q       = (S.data && idx >= S.minIndex && idx <= S.maxIndex) ? S.data + (idx - S.minIndex) * S.stride : NULL;
			}
			if (q) {
				const u32 cs = kCompSize[L.texType[i]];
				for (u32 k = 0; k < L.texCount[i]; k++) {
					out.tex[i][k] = readComp(q + k * cs, L.texType[i], texScale[i]);
				}
			}
		}
	}
}

u32 CommandProcessor::drawCommand(const u8* data, u32 size, bool inDisplayList)
{
	if (size < 3) {
		return 0;
	}
	const u8 op    = data[0];
	const u32 count = be16(data + 1);
	VertexLayout L;
	layoutFor(op & 7, L);
	const u32 need = 3 + count * L.size;
	if (size < need) {
		return 0;
	}
	mStats.draws++;
	mStats.vertices += count;
	mStats.primitives |= 1u << ((op >> 3) & 7);
	decodeVertices(L, data + 3, count);
	if (mListener && count <= mVertCap) {
		DrawCall call;
		call.primitive     = op & 0xF8;
		call.vat           = op & 7;
		call.count         = (u16)count;
		call.inDisplayList = inDisplayList;
		call.layout        = &L;
		call.vertices      = mVerts;
		mListener->draw(call);
	}
	return need;
}

// ---------------------------------------------------------------- textures

u32 CommandProcessor::textureSize(u32 format, u32 width, u32 height, u32 levels)
{
	u32 bw, bh, bytes;
	switch (format) {
	case TF_I4:
	case TF_C4:
	case TF_CMPR:
		bw = 8, bh = 8, bytes = 32;
		break;
	case TF_I8:
	case TF_IA4:
	case TF_C8:
		bw = 8, bh = 4, bytes = 32;
		break;
	case TF_RGBA8:
		bw = 4, bh = 4, bytes = 64;
		break;
	default: // IA8, RGB565, RGB5A3, C14X2
		bw = 4, bh = 4, bytes = 32;
		break;
	}
	u32 total = 0;
	for (u32 l = 0; l < levels; l++) {
		u32 w = width >> l, h = height >> l;
		w = w ? w : 1;
		h = h ? h : 1;
		total += ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bytes;
		if (w == 1 && h == 1) {
			break;
		}
	}
	return total;
}

void CommandProcessor::textureInfo(u32 map, u32& address, u32& width, u32& height, u32& format, u32& levels) const
{
	const u32 base   = map < 4 ? 0x80 + map : 0xA0 + (map - 4);
	const u32 mode0  = mState.bp[base + 0x00];
	const u32 mode1  = mState.bp[base + 0x04];
	const u32 image0 = mState.bp[base + 0x08];
	const u32 image3 = mState.bp[base + 0x14];
	width   = bits(image0, 0, 10) + 1;
	height  = bits(image0, 10, 10) + 1;
	format  = bits(image0, 20, 4);
	address = bits(image3, 0, 21) << 5;
	// Mipmaps: the minification filter uses them (hardware filter values 1, 2, 5, 6), up to max LOD.
	const u32 minFilter = bits(mode0, 5, 3);
	levels              = 1;
	if ((minFilter & 3) != 0) {
		levels = (bits(mode1, 8, 8) >> 4) + 1;
		if (levels > 11) {
			levels = 11;
		}
	}
}

} // namespace gpu
