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

const u8 kCompSize[8]  = { 1, 1, 2, 2, 4, 4, 4, 4 };
const u8 kColorSize[8] = { 2, 3, 4, 2, 3, 4, 4, 4 };

// Integer to float conversions by table: on the PS3's PPU a conversion goes through memory (a store and
// a load of the same address, which stalls the pipeline for dozens of cycles). 16-bit values are the
// sum of two exact parts: high byte * 256 + low byte.
static float sU8Float[256], sS8Float[256], sU16Hi[256], sS16Hi[256], sPow2Inv[32];

// How far ahead display lists are read (bytes).
enum { READ_AHEAD = 1024 };

// BP registers whose writes do something besides setting the register (loadBp).
static u8 sBpActs[256];

} // namespace gpu

extern "C" {
volatile unsigned int ps3GfxPhase;
unsigned long long ps3GfxPhaseTicks[gpu::GFX_PHASE_COUNT];
unsigned long long ps3GfxPhaseSince;
}

namespace gpu {
const char* const kGfxPhaseNames[GFX_PHASE_COUNT] = { "idle", "commands", "decode", "state", "textures", "programs", "xf set-up",
	                                                  "transform", "triangles", "rsx commands", "rsx waits", "copies", "checks" };
}

namespace gpu {

CommandProcessor::CommandProcessor()
    : mMemory(NULL)
    , mListener(NULL)
    , mRegs(NULL)
    , mBpMask(0xFFFFFF)
    , mDisplayListDepth(0)
    , mRuns(false)
    , mPendingBuf(NULL)
    , mPending(0)
    , mPendingCap(0)
    , mReceived(0)
    , mCmdStart(0)
    , mCmdEnd(0)
    , mLayoutValid(0)
    , mVerts(NULL)
    , mVertCap(0)
    , mLayoutVersion(0)
{
	memset(&mState, 0, sizeof(mState));
	memset(&mStats, 0, sizeof(mStats));
	memset(mLayouts, 0, sizeof(mLayouts));
	memset(mArrays, 0, sizeof(mArrays));
	for (int i = 0; i < 256; i++) {
		sU8Float[i] = (float)i;
		sS8Float[i] = (float)(s8)i;
		sU16Hi[i]   = (float)i * 256.0f;
		sS16Hi[i]   = (float)(s8)i * 256.0f;
	}
	for (int i = 0; i < 32; i++) {
		sPow2Inv[i] = 1.0f / (float)(1u << i);
	}
	// draw done (and its interrupt enable), tokens, EFB copy, TMEM preload and TLUT load, texture cache
	// invalidation, performance counters, the mask
	static const u8 kActs[] = { 0x45, 0x46, 0x47, 0x48, 0x52, 0x63, 0x64, 0x65, 0x66, 0x67, 0xFE, 0xFF };
	for (u32 i = 0; i < sizeof(kActs); i++) {
		sBpActs[kActs[i]] = 1;
	}
}

CommandProcessor::~CommandProcessor()
{
	free(mPendingBuf);
	free(mVerts);
}

// ---------------------------------------------------------------- FIFO input

u32 CommandProcessor::commandLength(const u8* p, u32 avail)
{
	switch (p[0]) {
	case OP_NOP:
	case OP_INVAL_VCACHE:
		return 1;
	case OP_LOAD_CP:
		return 6;
	case OP_LOAD_XF:
		return avail < 5 ? 0 : 5 + (((be32(p + 1) >> 16) & 0xF) + 1) * 4;
	case OP_LOAD_INDX_A:
	case OP_LOAD_INDX_B:
	case OP_LOAD_INDX_C:
	case OP_LOAD_INDX_D:
	case OP_LOAD_BP:
		return 5;
	case OP_CALL_DL:
		return 9;
	default:
		if (p[0] >= OP_DRAW_FIRST && p[0] < 0xC0) {
			return avail < 3 ? 0 : 3 + be16(p + 1) * layout(p[0] & 7).size;
		}
		return 1; // unknown: reported, and skipped a byte at a time
	}
}

// Keeps bytes of an incomplete command until the rest arrives.
void CommandProcessor::hold(const u8* bytes, u32 n)
{
	if (mPending + n > mPendingCap) {
		u32 cap = mPendingCap ? mPendingCap * 2 : 4096;
		while (cap < mPending + n) {
			cap *= 2;
		}
		u8* buf = (u8*)realloc(mPendingBuf, cap);
		if (!buf) {
			mStats.badCommands++; // out of memory: the command is lost
			mPending = 0;
			return;
		}
		mPendingBuf = buf;
		mPendingCap = cap;
	}
	memcpy(mPendingBuf + mPending, bytes, n);
	mPending += n;
}

void CommandProcessor::write(const u8* data, u32 size)
{
	const u64 base = mReceived;
	mReceived += size;
	u32 pos = 0;
	if (mPending) {
		// Complete the command held from the last write with exactly the bytes it still needs.
		for (;;) {
			u32 need = commandLength(mPendingBuf, mPending);
			u32 want = need ? need - mPending : 1; // (header still incomplete: one byte at a time)
			if (pos >= size) {
				return;
			}
			u32 take = want < size - pos ? want : size - pos;
			hold(data + pos, take);
			pos += take;
			if (!mPending) {
				break; // (lost: out of memory)
			}
			if (need && mPending == need) {
				u32 n     = mPending;
				mPending  = 0;
				mCmdEnd   = base + pos;
				mCmdStart = mCmdEnd - n;
				run(mPendingBuf, n, false, mCmdStart);
				break;
			}
		}
	}
	u32 used = run(data + pos, size - pos, false, base + pos);
	pos += used;
	if (pos < size) {
		hold(data + pos, size - pos);
	}
}

void CommandProcessor::write8(u32 value)
{
	u8 b = (u8)value;
	write(&b, 1);
}

void CommandProcessor::write16(u32 value)
{
	u8 b[2] = { (u8)(value >> 8), (u8)value };
	write(b, 2);
}

void CommandProcessor::write32(u32 value)
{
	u8 b[4] = { (u8)(value >> 24), (u8)(value >> 16), (u8)(value >> 8), (u8)value };
	write(b, 4);
}

// ---------------------------------------------------------------- commands

u32 CommandProcessor::run(const u8* data, u32 size, bool inDisplayList, u64 base)
{
	// One switch per command: its length, its checks and its work. (Test builds up to 40 looked at each
	// command's opcode three times: is it known, how long is it, what does it do. The PPU predicts none of
	// those jumps.) Counters are kept in locals and added at the end: a counter in memory, stored and loaded
	// straight back by the next command, stalls the PPU's pipeline every time (a load-hit-store).
	u32 pos      = 0;
	u32 commands = 0, bpLoads = 0, xfLoads = 0, cpLoads = 0, indexedLoads = 0;
	while (pos < size) {
		const u8* p     = data + pos;
		const u32 avail = size - pos;
		const u32 op    = p[0];
		u32 len;
		if (inDisplayList && avail > READ_AHEAD) {
			__builtin_prefetch(p + READ_AHEAD);
		}
		switch (op) {
		case OP_NOP: // (GXFlush pads with 32 of them, display lists end with them: skipped together)
		case OP_INVAL_VCACHE:
		{
			u32 n = 1;
			while (n < avail && p[n] == OP_NOP) {
				n++;
			}
			commands += n;
			pos += n;
			continue;
		}
		case OP_LOAD_BP:
			if (avail < 5) {
				goto incomplete;
			}
			len = 5;
			if (!inDisplayList) {
				mCmdStart = base + pos;
				mCmdEnd   = base + pos + len;
			}
			bpLoads++;
			loadBp(be32(p + 1));
			break;
		case OP_LOAD_XF:
			if (avail < 5) {
				goto incomplete;
			}
			len = 5 + (((be32(p + 1) >> 16) & 0xF) + 1) * 4;
			if (len > avail) {
				goto incomplete;
			}
			if (!inDisplayList) {
				mCmdStart = base + pos;
				mCmdEnd   = base + pos + len;
			}
			xfLoads++;
			loadXf(be16(p + 3), ((p[2] & 0xF) + 1), p + 5);
			break;
		case OP_LOAD_CP:
			if (avail < 6) {
				goto incomplete;
			}
			len = 6;
			if (!inDisplayList) {
				mCmdStart = base + pos;
				mCmdEnd   = base + pos + len;
			}
			cpLoads++;
			loadCp(p[1], be32(p + 2));
			break;
		case OP_LOAD_INDX_A:
		case OP_LOAD_INDX_B:
		case OP_LOAD_INDX_C:
		case OP_LOAD_INDX_D:
			if (avail < 5) {
				goto incomplete;
			}
			len = 5;
			if (!inDisplayList) {
				mCmdStart = base + pos;
				mCmdEnd   = base + pos + len;
			}
			indexedLoads++;
			loadIndexed(op, be32(p + 1));
			break;
		case OP_CALL_DL:
			if (avail < 9) {
				goto incomplete;
			}
			len = 9;
			if (inDisplayList) {
				mStats.nestedDisplayLists++; // not supported by the hardware either
			} else {
				mCmdStart = base + pos;
				mCmdEnd   = base + pos + len;
				callDisplayList(be32(p + 1), be32(p + 5));
			}
			break;
		default:
			if (op >= OP_DRAW_FIRST && op < 0xC0) {
				if (avail < 3) {
					goto incomplete;
				}
				len = 3 + be16(p + 1) * layout(op & 7).size;
				if (len > avail) {
					goto incomplete;
				}
				if (!inDisplayList) {
					mCmdStart = base + pos;
					mCmdEnd   = base + pos + len;
				} else if (mRuns && op < PRIM_LINES) {
					// (a triangle draw: with the ones right after it, if any, one call)
					const u32 n = drawRun(p, avail);
					if (n) {
						pos += n;
						continue;
					}
				}
				drawCommand(p, inDisplayList);
				break;
			}
			mStats.badCommands++;
			if (mListener) {
				mListener->badCommand((u8)op, p, avail, inDisplayList);
			}
			len = 1; // (skipped a byte at a time: the hardware would hang)
			break;
		}
		commands++;
		pos += len;
	}
	mStats.commands += commands;
	mStats.bpLoads += bpLoads;
	mStats.xfLoads += xfLoads;
	mStats.cpLoads += cpLoads;
	mStats.indexedLoads += indexedLoads;
	return pos;

incomplete:
	mStats.commands += commands;
	mStats.bpLoads += bpLoads;
	mStats.xfLoads += xfLoads;
	mStats.cpLoads += cpLoads;
	mStats.indexedLoads += indexedLoads;
	if (inDisplayList) {
		mStats.badCommands++;
		if (mListener) {
			mListener->badCommand(data[pos], data + pos, size - pos, true);
		}
		return size;
	}
	return pos;
}

// Register loads that change nothing (the game sets most of its state again for every material, the GX
// library writes some registers several times over, and test build 40's busiest frames had 9,000 of
// 19,000 BP loads and 5,000 of 10,000 XF loads like that) are not passed on: the renderer would redo its
// state work for each. BP registers whose writes act (draw done, tokens, copies, TLUT loads, texture cache
// invalidation, the mask) are passed on every time.

void CommandProcessor::loadCp(u32 reg, u32 value)
{
	reg &= 0xFF;
	if (mState.cp[reg] == value) {
		mStats.cpUnchanged++;
		return;
	}
	mState.cp[reg] = value;
	if (reg == 0x50 || reg == 0x60) {
		mLayoutValid = 0; // vertex descriptor: all formats
	} else if (reg >= 0x70 && reg < 0xA0 && (reg & 0xF) < 8) {
		mLayoutValid &= (u8)~(1u << (reg & 7)); // VAT A/B/C of one format
	}
}

void CommandProcessor::loadXf(u32 address, u32 count, const u8* values)
{
	u32 changed = 0;
	for (u32 i = 0; i < count; i++) {
		const u32 a = address + i;
		if (a < sizeof(mState.xf) / sizeof(mState.xf[0])) {
			const u32 v = be32(values + i * 4);
			changed |= mState.xf[a] ^ v;
			mState.xf[a] = v;
		} else {
			mStats.xfOutOfRange++;
		}
	}
	if (!changed) {
		mStats.xfUnchanged++;
		return;
	}
	if (address + count > 0x600) {
		mStats.xfSetupLoads++; // (lights or registers: not just matrices)
	}
	if (mRegs) {
		mRegs->xfWritten(address, count);
	}
}

void CommandProcessor::loadIndexed(u32 op, u32 word)
{
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
	u32 changed = 0;
	for (u32 i = 0; i < count; i++) {
		u32 a = xfAddr + i;
		if (a < sizeof(mState.xf) / sizeof(mState.xf[0])) {
			const u32 v = be32(src + i * 4);
			changed |= mState.xf[a] ^ v;
			mState.xf[a] = v;
		}
	}
	if (!changed) {
		mStats.xfUnchanged++;
		return;
	}
	if (xfAddr + count > 0x600) {
		mStats.xfSetupLoads++;
	}
	if (mRegs) {
		mRegs->xfWritten(xfAddr, count);
	}
}

void CommandProcessor::loadBp(u32 word)
{
	const u32 reg = word >> 24;
	u32 value     = word & 0xFFFFFF;
	if (reg == 0xFE) {
		mBpMask = value; // applies to the next BP write only
		return;
	}
	const u32 old = mState.bp[reg];
	if (mBpMask != 0xFFFFFF) {
		value   = (old & ~mBpMask) | (value & mBpMask);
		mBpMask = 0xFFFFFF; // (stored only when it was used: stored every time, the next load of it would stall)
	}
	if (value == old && !sBpActs[reg]) {
		mStats.bpUnchanged++;
		return;
	}
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
	if (mRegs) {
		mRegs->bpWritten(reg, value);
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
	// (display lists are read from memory, not the cache, each frame: read ahead, here and as they run. The
	// PPU waits hundreds of cycles for each line that is not: ask for the first lines together.)
	for (u32 at = 0; at < size && at < READ_AHEAD; at += 128) {
		__builtin_prefetch(list + at);
	}
	mDisplayListDepth++;
	mRuns = mListener && mListener->drawRuns() && mListener->vertexDetail() == VERTICES_NONE;
	run(list, size, true, 0);
	mRuns = false;
	mDisplayListDepth--;
}

// ---------------------------------------------------------------- vertex formats

// Where each attribute is in a stream vertex.
static void streamOffsets(VertexLayout& L)
{
	static const u8 kIndexSize[4] = { 0, 0, 1, 2 };
	u32 off = L.pnmtx;
	for (int i = 0; i < 8; i++) {
		L.offTexMtx[i] = (u16)off;
		off += L.texmtx[i];
	}
	L.offPos = (u16)off;
	off += L.posMode == ATTR_DIRECT ? L.posCount * kCompSize[L.posType] : kIndexSize[L.posMode];
	L.offNrm           = (u16)off;
	const u32 nrmIdxN = (L.nrmNbt && L.nrmIndex3) ? 3 : 1;
	if (L.nrmMode == ATTR_DIRECT) {
		off += (L.nrmNbt ? 9 : 3) * kCompSize[L.nrmType];
	} else {
		off += kIndexSize[L.nrmMode] * nrmIdxN;
	}
	for (int i = 0; i < 2; i++) {
		L.offClr[i] = (u16)off;
		off += L.clrMode[i] == ATTR_DIRECT ? kColorSize[L.clrFormat[i]] : kIndexSize[L.clrMode[i]];
	}
	for (int i = 0; i < 8; i++) {
		L.offTex[i] = (u16)off;
		off += L.texMode[i] == ATTR_DIRECT ? L.texCount[i] * kCompSize[L.texType[i]] : kIndexSize[L.texMode[i]];
	}
}

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
	streamOffsets(L);

	// attributes from arrays
	u32 n = 0;
	const u32 nrmIdxN = (L.nrmNbt && L.nrmIndex3) ? 3 : 1;
	const u32 nrmElem = (L.nrmNbt && !L.nrmIndex3) ? 9 : 3;
	if (L.posMode >= ATTR_INDEX8) {
		VertexLayout::Indexed x = { ARRAY_POS, L.posMode, 1, L.offPos, (u16)(L.posCount * kCompSize[L.posType]) };
		L.indexed[n++] = x;
	}
	if (L.nrmMode >= ATTR_INDEX8) {
		VertexLayout::Indexed x = { ARRAY_NRM, L.nrmMode, (u8)nrmIdxN, L.offNrm, (u16)(nrmElem * kCompSize[L.nrmType]) };
		L.indexed[n++] = x;
	}
	for (int i = 0; i < 2; i++) {
		if (L.clrMode[i] >= ATTR_INDEX8) {
			VertexLayout::Indexed x = { (u8)(ARRAY_CLR0 + i), L.clrMode[i], 1, L.offClr[i], kColorSize[L.clrFormat[i]] };
			L.indexed[n++] = x;
		}
	}
	for (int i = 0; i < 8; i++) {
		if (L.texMode[i] >= ATTR_INDEX8) {
			VertexLayout::Indexed x = { (u8)(ARRAY_TEX0 + i), L.texMode[i], 1, L.offTex[i], (u16)(L.texCount[i] * kCompSize[L.texType[i]]) };
			L.indexed[n++] = x;
		}
	}
	L.indexedCount = (u8)n;
}

// ---------------------------------------------------------------- vertex decoding

static inline float readComp(const u8* p, u32 type, float scale)
{
	switch (type) {
	case COMP_U8: return sU8Float[p[0]] * scale;
	case COMP_S8: return sS8Float[p[0]] * scale;
	case COMP_U16: return (sU16Hi[p[0]] + sU8Float[p[1]]) * scale;
	case COMP_S16: return (sS16Hi[p[0]] + sU8Float[p[1]]) * scale;
	default:
	{
#if defined(__BIG_ENDIAN__)
		if (((size_t)p & 3) == 0) {
			return *(const float*)p; // (aligned: straight into a floating-point register)
		}
#endif
		union {
			u32 u;
			float f;
		} v;
		v.u = be32(p);
		return v.f;
	}
	}
}

void readColor(const u8* p, u32 format, u8* out)
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


void CommandProcessor::decodeVertices(const VertexLayout& L, const u8* data, u32 count, bool decode)
{
	PhaseScope phase(GFX_DECODE);
	if (decode && count > mVertCap) {
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

	// Pass 1: the index ranges of indexed attributes.
	static const u8 kIndexSize[4] = { 0, 0, 1, 2 };
	const u32 posOff  = L.offPos;
	const u32 nrmOff  = L.offNrm;
	const u16* clrOff = L.offClr;
	const u16* texOff = L.offTex;
	const u16* texMtxOff = L.offTexMtx;

	VertexArray* src = mArrays;
	for (int k = 0; k < 12; k++) {
		src[k].data = NULL, src[k].used = false;
	}
	for (u32 k = 0; k < L.indexedCount; k++) {
		const VertexLayout::Indexed& A = L.indexed[k];
		VertexArray& S                 = src[A.array];
		u32 lo = 0xFFFFFFFF, hi = 0;
		const u32 isz   = kIndexSize[A.mode];
		const u32 skip  = A.array == ARRAY_POS ? (isz == 1 ? 0xFFu : 0xFFFFu) : 0xFFFFFFFFu; // ("skip this vertex")
		const u8* p     = data + A.offset;
		if (A.indices == 1 && isz == 1) {
			for (u32 v = 0; v < count; v++, p += L.size) {
				const u32 idx = p[0];
				if (idx != skip) {
					lo = idx < lo ? idx : lo;
					hi = idx > hi ? idx : hi;
				}
			}
		} else if (A.indices == 1) {
			for (u32 v = 0; v < count; v++, p += L.size) {
				const u32 idx = ((u32)p[0] << 8) | p[1];
				if (idx != skip) {
					lo = idx < lo ? idx : lo;
					hi = idx > hi ? idx : hi;
				}
			}
		} else {
			for (u32 v = 0; v < count; v++, p += L.size) {
				for (u32 n = 0; n < A.indices; n++) {
					const u32 idx = isz == 1 ? p[n] : be16(p + n * 2);
					lo = idx < lo ? idx : lo;
					hi = idx > hi ? idx : hi;
				}
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

	if (!decode) {
		return;
	}

	// Pass 2: decode.
	const u32 matA = mState.cp[0x30];
	const u32 matB = mState.cp[0x40];
	const float posScale = (L.posType == COMP_F32) ? 1.0f : sPow2Inv[L.posFrac & 31];
	const float nrmScale = (L.nrmType == COMP_S8 || L.nrmType == COMP_U8) ? 1.0f / 64.0f : 1.0f / 16384.0f;
	float texScale[8];
	for (int i = 0; i < 8; i++) {
		texScale[i] = (L.texType[i] == COMP_F32) ? 1.0f : sPow2Inv[L.texFrac[i] & 31];
	}

	// (per draw: the default matrix indices, and which attributes are present)
	const u8 defPn = (u8)bits(matA, 0, 6);
	u8 defTex[8];
	bool anyTexMtx = false;
	for (int i = 0; i < 8; i++) {
		defTex[i] = (u8)(i < 4 ? bits(matA, 6 + i * 6, 6) : bits(matB, (i - 4) * 6, 6));
		anyTexMtx |= L.texmtx[i] != 0;
	}
	int texList[8], texN = 0;
	for (int i = 0; i < 8; i++) {
		if (L.texMode[i] != ATTR_NONE) {
			texList[texN++] = i;
		}
	}
	const bool hasNbt = L.nrmMode != ATTR_NONE && L.nrmNbt;

	for (u32 v = 0; v < count; v++) {
		const u8* p = data + v * L.size;
		Vertex& out = mVerts[v];
		// (fields this layout does not set are zero)
		if (L.posMode == ATTR_NONE) {
			out.pos[0] = out.pos[1] = 0;
		}
		out.pos[2] = 0;
		if (L.nrmMode == ATTR_NONE) {
			out.nrm[0] = out.nrm[1] = out.nrm[2] = 0;
		}
		if (!hasNbt) {
			out.binormal[0] = out.binormal[1] = out.binormal[2] = 0;
			out.tangent[0] = out.tangent[1] = out.tangent[2] = 0;
		}
		for (int i = 0; i < 2; i++) {
			if (L.clrMode[i] == ATTR_NONE) {
				out.clr[i][0] = out.clr[i][1] = out.clr[i][2] = out.clr[i][3] = 0;
			}
		}
		memset(out.tex, 0, sizeof(out.tex));

		out.pnmtx = L.pnmtx ? (p[0] & 63) : defPn;
		if (anyTexMtx) {
			for (int i = 0; i < 8; i++) {
				out.texmtx[i] = L.texmtx[i] ? (p[texMtxOff[i]] & 63) : defTex[i];
			}
		} else {
			memcpy(out.texmtx, defTex, 8);
		}

		// position
		if (L.posMode != ATTR_NONE) {
			const u8* q = NULL;
			if (L.posMode == ATTR_DIRECT) {
				q = p + posOff;
			} else {
				const VertexArray& S = src[ARRAY_POS];
				u32 idx = L.posMode == ATTR_INDEX8 ? p[posOff] : be16(p + posOff);
				q       = (S.data && idx >= S.minIndex && idx <= S.maxIndex) ? S.data + (idx - S.minIndex) * S.stride : NULL;
			}
			if (q) {
				const u32 cs = kCompSize[L.posType];
				for (u32 k = 0; k < L.posCount; k++) {
					out.pos[k] = readComp(q + k * cs, L.posType, posScale);
				}
			} else {
				out.pos[0] = out.pos[1] = 0;
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
					const VertexArray& S = src[ARRAY_NRM];
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
				} else {
					dst[n][0] = dst[n][1] = dst[n][2] = 0;
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
				const VertexArray& S = src[ARRAY_CLR0 + i];
				u32 idx = L.clrMode[i] == ATTR_INDEX8 ? p[clrOff[i]] : be16(p + clrOff[i]);
				q       = (S.data && idx >= S.minIndex && idx <= S.maxIndex) ? S.data + (idx - S.minIndex) * S.stride : NULL;
			}
			if (q) {
				readColor(q, L.clrFormat[i], out.clr[i]);
			} else {
				out.clr[i][0] = out.clr[i][1] = out.clr[i][2] = out.clr[i][3] = 0;
			}
		}

		// texture coordinates
		for (int ti = 0; ti < texN; ti++) {
			const int i = texList[ti];
			const u8* q = NULL;
			if (L.texMode[i] == ATTR_DIRECT) {
				q = p + texOff[i];
			} else {
				const VertexArray& S = src[ARRAY_TEX0 + i];
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

// ---------------------------------------------------------------- vertices for a vertex program

namespace {

// The attribute's data for the vertex at p: in the stream, or the array element its index picks.
inline const u8* element(u32 mode, const u8* at, const VertexArray& A)
{
	if (mode == ATTR_DIRECT) {
		return at;
	}
	const u32 idx = mode == ATTR_INDEX8 ? at[0] : ((u32)at[0] << 8 | at[1]);
	return (A.data && idx >= A.minIndex && idx <= A.maxIndex) ? A.data + (idx - A.minIndex) * A.stride : NULL;
}

} // namespace

void CommandProcessor::matrixRows(const DrawCall& call, u32 texCount, u64* pos, u64* tex) const
{
	const VertexLayout& L = *call.layout;
	const u32 matA = mState.cp[0x30];
	const u32 matB = mState.cp[0x40];
	if (L.pnmtx) {
		u64 m = 0;
		for (u32 v = 0; v < call.count; v++) {
			m |= 1ULL << (call.data[v * L.size] & 63);
		}
		*pos = m;
	} else {
		*pos = 1ULL << bits(matA, 0, 6);
	}
	for (u32 n = 0; n < texCount && n < 8; n++) {
		if (L.texmtx[n]) {
			u64 m = 0;
			for (u32 v = 0; v < call.count; v++) {
				m |= 1ULL << (call.data[v * L.size + L.offTexMtx[n]] & 63);
			}
			tex[n] = m;
		} else {
			tex[n] = 1ULL << (n < 4 ? bits(matA, 6 + n * 6, 6) : bits(matB, (n - 4) * 6, 6));
		}
	}
}

namespace {

template <u32 TYPE> inline float component(const u8* p, float scale);
template <> inline float component<COMP_U8>(const u8* p, float scale) { return sU8Float[p[0]] * scale; }
template <> inline float component<COMP_S8>(const u8* p, float scale) { return sS8Float[p[0]] * scale; }
template <> inline float component<COMP_U16>(const u8* p, float scale) { return (sU16Hi[p[0]] + sU8Float[p[1]]) * scale; }
template <> inline float component<COMP_S16>(const u8* p, float scale) { return (sS16Hi[p[0]] + sU8Float[p[1]]) * scale; }
template <> inline float component<COMP_F32>(const u8* p, float scale) { return readComp(p, COMP_F32, scale); }

// One attribute of every vertex as floats: `comps` components read, `want` written (missing ones 0).
template <u32 TYPE>
void gatherFloats(const u8* data, u32 size, u32 count, u32 mode, u32 off, const VertexArray& A, u32 comps, u32 want, float scale,
                  u8* out, u32 stride)
{
	const u32 cs = TYPE == COMP_U8 || TYPE == COMP_S8 ? 1 : TYPE == COMP_U16 || TYPE == COMP_S16 ? 2 : 4;
	const u8* p  = data + off;
	for (u32 v = 0; v < count; v++, p += size, out += stride) {
		// (array elements a few vertices ahead are fetched into the cache meanwhile: they are scattered,
		// and a cache miss costs the PS3 hundreds of cycles)
		if (mode >= ATTR_INDEX8 && v + 8 < count) {
			const u8* e = element(mode, p + 8 * size, A);
			if (e) {
				__builtin_prefetch(e);
			}
		}
		const u8* q = element(mode, p, A);
		float* f    = (float*)out;
		if (q) {
			f[0] = component<TYPE>(q, scale);
			f[1] = comps > 1 ? component<TYPE>(q + cs, scale) : 0.0f;
			if (want > 2) {
				f[2] = comps > 2 ? component<TYPE>(q + 2 * cs, scale) : 0.0f;
			}
		} else {
			f[0] = f[1] = 0.0f;
			if (want > 2) {
				f[2] = 0.0f;
			}
		}
	}
}

void gatherAttribute(const u8* data, u32 size, u32 count, u32 mode, u32 type, u32 off, const VertexArray& A, u32 comps, u32 want,
                     float scale, u8* out, u32 stride)
{
	if (mode == ATTR_NONE) {
		for (u32 v = 0; v < count; v++, out += stride) {
			float* f = (float*)out;
			f[0] = f[1] = 0.0f;
			if (want > 2) {
				f[2] = 0.0f;
			}
		}
		return;
	}
	switch (type) {
	case COMP_U8: gatherFloats<COMP_U8>(data, size, count, mode, off, A, comps, want, scale, out, stride); break;
	case COMP_S8: gatherFloats<COMP_S8>(data, size, count, mode, off, A, comps, want, scale, out, stride); break;
	case COMP_U16: gatherFloats<COMP_U16>(data, size, count, mode, off, A, comps, want, scale, out, stride); break;
	case COMP_S16: gatherFloats<COMP_S16>(data, size, count, mode, off, A, comps, want, scale, out, stride); break;
	default: gatherFloats<COMP_F32>(data, size, count, mode, off, A, comps, want, scale, out, stride); break;
	}
}

} // namespace

void CommandProcessor::gatherVertices(const DrawCall& call, const GatherSpec& G, u8* out) const
{
	PhaseScope phase(GFX_DECODE);
	const VertexLayout& L = *call.layout;
	const VertexArray* A  = call.arrays;
	const u32 count       = call.count;
	const u32 size        = L.size;
	const u8* data        = call.data;
	const u32 stride      = G.stride;

	// matrix indices (attribute by attribute: the PS3's processor predicts the branches)
	if (G.indexCount && G.indexFloat) {
		const u32 matA = mState.cp[0x30];
		const u32 matB = mState.cp[0x40];
		for (u32 t = 0; t < G.indexCount; t++) {
			u8* ix      = out + G.index + t * 4;
			const u8* p = data + (t ? L.offTexMtx[t - 1] : 0);
			const bool fromVertex = t ? L.texmtx[t - 1] != 0 : L.pnmtx != 0;
			const u8* map         = t ? G.texMap : G.posMap;
			if (fromVertex) {
				for (u32 v = 0; v < count; v++, p += size, ix += stride) {
					*(float*)ix = sU8Float[map[p[0] & 63]];
				}
			} else {
				const u32 n   = t - 1;
				const u32 row = t ? (n < 4 ? bits(matA, 6 + n * 6, 6) : bits(matB, (n - 4) * 6, 6)) : bits(matA, 0, 6);
				const float f = sU8Float[map[row]];
				for (u32 v = 0; v < count; v++, ix += stride) {
					*(float*)ix = f;
				}
			}
		}
	} else if (G.indexCount) {
		const u32 matA = mState.cp[0x30];
		const u32 matB = mState.cp[0x40];
		const u8 pn    = G.posMap[bits(matA, 0, 6)];
		u8* ix         = out + G.index;
		const u8* p    = data;
		for (u32 v = 0; v < count; v++, p += size, ix += stride) {
			ix[0] = L.pnmtx ? G.posMap[p[0] & 63] : pn;
		}
		for (u32 t = 1; t < G.indexCount; t++) {
			const u32 n   = t - 1;
			ix            = out + G.index + t;
			p             = data + L.offTexMtx[n];
			if (L.texmtx[n]) {
				for (u32 v = 0; v < count; v++, p += size, ix += stride) {
					ix[0] = G.texMap[p[0] & 63];
				}
			} else {
				const u8 m = G.texMap[n < 4 ? bits(matA, 6 + n * 6, 6) : bits(matB, (n - 4) * 6, 6)];
				for (u32 v = 0; v < count; v++, ix += stride) {
					ix[0] = m;
				}
			}
		}
	}
	// position
	gatherAttribute(data, size, count, L.posMode, L.posType, L.offPos, A[ARRAY_POS], L.posCount, 3,
	                L.posType == COMP_F32 ? 1.0f : sPow2Inv[L.posFrac & 31], out + G.pos, stride);
	// normal
	if (G.nrm != GatherSpec::NO_OFFSET) {
		gatherAttribute(data, size, count, L.nrmMode, L.nrmType, L.offNrm, A[ARRAY_NRM], 3, 3,
		                (L.nrmType == COMP_S8 || L.nrmType == COMP_U8) ? 1.0f / 64.0f : 1.0f / 16384.0f, out + G.nrm, stride);
	}
	// colors
	for (int i = 0; i < 2; i++) {
		if (G.clr[i] == GatherSpec::NO_OFFSET) {
			continue;
		}
		u8* c       = out + G.clr[i];
		const u8* p = data + L.offClr[i];
		const u32 mode = L.clrMode[i], format = L.clrFormat[i];
		const VertexArray& CA = A[ARRAY_CLR0 + i];
		for (u32 v = 0; v < count; v++, p += size, c += stride) {
			const u8* q = mode != ATTR_NONE ? element(mode, p, CA) : NULL;
			if (q) {
				readColor(q, format, c);
			} else {
				c[0] = c[1] = c[2] = c[3] = 0;
			}
		}
	}
	// texture coordinates
	for (int i = 0; i < 8; i++) {
		if (G.tex[i] != GatherSpec::NO_OFFSET) {
			gatherAttribute(data, size, count, L.texMode[i], L.texType[i], L.offTex[i], A[ARRAY_TEX0 + i], L.texCount[i], 2,
			                L.texType[i] == COMP_F32 ? 1.0f : sPow2Inv[L.texFrac[i] & 31], out + G.tex[i], stride);
		}
	}
}

const Vertex* CommandProcessor::decodeDraw(const DrawCall& call)
{
	decodeVertices(*call.layout, call.data, call.count, true);
	return call.count <= mVertCap ? mVerts : NULL;
}

const VertexArray* CommandProcessor::resolveArrays(const DrawCall& call)
{
	decodeVertices(*call.layout, call.data, call.count, false);
	return mArrays;
}

void CommandProcessor::drawCommand(const u8* data, bool inDisplayList)
{
	const u8 op           = data[0];
	const u32 count       = be16(data + 1);
	const VertexLayout& L = layout(op & 7);
	mStats.draws++;
	mStats.vertices += count;
	mStats.primitives |= 1u << ((op >> 3) & 7);
	const VertexDetail detail = mListener ? mListener->vertexDetail() : VERTICES_NONE;
	if (detail != VERTICES_NONE) {
		decodeVertices(L, data + 3, count, detail == VERTICES_DECODED);
	}
	if (mListener) {
		DrawCall call;
		call.primitive     = op & 0xF8;
		call.vat           = op & 7;
		call.count         = (u16)count;
		call.inDisplayList = inDisplayList;
		call.parts         = 1;
		call.layout        = &L;
		call.vertices      = (detail == VERTICES_DECODED && count <= mVertCap) ? mVerts : NULL;
		call.data          = data + 3;
		call.arrays        = detail != VERTICES_NONE ? mArrays : NULL;
		mListener->draw(call);
	}
}

// A display list's triangle draw at p (complete: avail bytes there) and the triangle draws with the same
// vertex format right after it, as one call (Listener::drawRuns). Returns the bytes they take, or 0 for a
// single draw (executed as usual).
u32 CommandProcessor::drawRun(const u8* p, u32 avail)
{
	enum { RUN_BYTES = 64 * 1024 }; // (a listener keeps a run's bytes to compare: in pieces this size at most)
	const u32 vat         = p[0] & 7;
	const VertexLayout& L = layout(vat);
	const u32 size        = L.size;
	u32 total             = be16(p + 1);
	u32 at                = 3 + total * size;
	u32 parts             = 1;
	u32 prims             = 1u << ((p[0] >> 3) & 7);
	while (avail - at >= 3) {
		const u8* q  = p + at;
		const u32 op = q[0];
		if (op < PRIM_QUADS || op >= PRIM_LINES || (op & 7) != vat) {
			break;
		}
		const u32 n   = be16(q + 1);
		const u32 len = 3 + n * size;
		if (len > avail - at || total + n > 0xFFFF || at + len > RUN_BYTES) {
			break;
		}
		__builtin_prefetch(q + READ_AHEAD);
		total += n;
		at += len;
		parts++;
		prims |= 1u << ((op >> 3) & 7);
	}
	if (parts == 1) {
		return 0;
	}
	mStats.draws += parts;
	mStats.commands += parts;
	mStats.vertices += total;
	mStats.primitives |= prims;
	DrawCall call;
	call.primitive     = p[0] & 0xF8;
	call.vat           = (u8)vat;
	call.count         = (u16)total;
	call.inDisplayList = 1;
	call.parts         = (u16)parts;
	call.layout        = &L;
	call.vertices      = NULL;
	call.data          = p + 3;
	call.arrays        = NULL;
	mListener->draw(call);
	return at;
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
