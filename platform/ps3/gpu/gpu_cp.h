// PS3 port: the GameCube graphics processor's command interface, in software.
//
// Nintendo's GX library (src/gx) turns the game's drawing calls into the command stream the GameCube's
// graphics processor ("Flipper") reads from its FIFO: loads of its CP (vertex format), XF (transform) and
// BP (rasterizer, texture, TEV, pixel) registers, display list calls, and primitives with their vertex
// data. This file interprets that stream: it keeps the register state, decodes vertices (from the stream
// and from the vertex arrays in memory), and reports draws, copies and synchronization events to a
// Listener. Rendering (RSX on the PS3, a software rasterizer in the host tools) is done by listeners.
//
// The command and register formats follow the GameCube's hardware as documented by the homebrew
// community ("Yet Another GameCube Documentation") and the Dolphin emulator, and as the GX library in
// src/gx encodes them. This is a separate implementation.
//
// Portable C++ (no PS3 or game headers), so the same code runs on the console and in host tools.

#ifndef PS3_GPU_CP_H
#define PS3_GPU_CP_H

#include <stddef.h>

namespace gpu {

// (The PS3 build compiles with the game's own C library headers, which have no stdint.h.)
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;

// ---------------------------------------------------------------- command opcodes

enum {
	OP_NOP          = 0x00,
	OP_LOAD_CP      = 0x08,
	OP_LOAD_XF      = 0x10,
	OP_LOAD_INDX_A  = 0x20, // position matrices
	OP_LOAD_INDX_B  = 0x28, // normal matrices
	OP_LOAD_INDX_C  = 0x30, // texture matrices
	OP_LOAD_INDX_D  = 0x38, // lights
	OP_CALL_DL      = 0x40,
	OP_UNKNOWN_44   = 0x44,
	OP_INVAL_VCACHE = 0x48,
	OP_LOAD_BP      = 0x61,
	OP_DRAW_FIRST   = 0x80, // 0x80-0xBF: primitive (bits 3-5) | vertex format (bits 0-2)
};

enum Primitive {
	PRIM_QUADS     = 0x80,
	PRIM_QUADS_2   = 0x88,
	PRIM_TRIANGLES = 0x90,
	PRIM_TRI_STRIP = 0x98,
	PRIM_TRI_FAN   = 0xA0,
	PRIM_LINES     = 0xA8,
	PRIM_LINE_STRIP = 0xB0,
	PRIM_POINTS    = 0xB8,
};

// CP vertex arrays (CP registers 0xA0+n / 0xB0+n)
enum {
	ARRAY_POS  = 0,
	ARRAY_NRM  = 1,
	ARRAY_CLR0 = 2,
	ARRAY_CLR1 = 3,
	ARRAY_TEX0 = 4, // .. 11
	ARRAY_INDX_A = 12, // .. 15: matrices/lights for the indexed loads
	NUM_ARRAYS = 16,
};

// How an attribute is present in a vertex (VCD)
enum { ATTR_NONE = 0, ATTR_DIRECT = 1, ATTR_INDEX8 = 2, ATTR_INDEX16 = 3 };

// Component types (VAT) for positions, normals and texture coordinates
enum { COMP_U8 = 0, COMP_S8 = 1, COMP_U16 = 2, COMP_S16 = 3, COMP_F32 = 4 };

// Color formats (VAT)
enum { CLR_RGB565 = 0, CLR_RGB888 = 1, CLR_RGB888X = 2, CLR_RGBA4444 = 3, CLR_RGBA6666 = 4, CLR_RGBA8888 = 5 };

// Texture formats (TX_SETIMAGE0)
enum {
	TF_I4     = 0x0,
	TF_I8     = 0x1,
	TF_IA4    = 0x2,
	TF_IA8    = 0x3,
	TF_RGB565 = 0x4,
	TF_RGB5A3 = 0x5,
	TF_RGBA8  = 0x6,
	TF_C4     = 0x8,
	TF_C8     = 0x9,
	TF_C14X2  = 0xA,
	TF_CMPR   = 0xE,
};

// What a memory read is for; it tells the platform how many address bits the command kept.
enum MemKind {
	MEM_DISPLAY_LIST, // 32-bit address
	MEM_ARRAY,        // 30-bit (CP array base registers)
	MEM_TEXTURE,      // 26-bit (texture image, TLUT and copy addresses: 21 bits of address >> 5)
	MEM_TLUT,
	MEM_KIND_COUNT,
};

// ---------------------------------------------------------------- vertex layout and decoded vertices

struct VertexLayout {
	u8 pnmtx;        // a position/normal matrix index byte is present
	u8 texmtx[8];    // texture matrix index bytes present
	u8 posMode, posType, posCount, posFrac; // posCount: 2 or 3
	u8 nrmMode, nrmType, nrmNbt, nrmIndex3; // nrmNbt: normal, binormal, tangent
	u8 clrMode[2], clrFormat[2], clrHasAlpha[2];
	u8 texMode[8], texType[8], texCount[8], texFrac[8]; // texCount: 1 or 2
	u8 byteDequant;
	u32 size; // bytes per vertex in the command stream
};

struct Vertex {
	float pos[3];
	float nrm[3];
	float binormal[3];
	float tangent[3];
	u8 clr[2][4]; // RGBA
	float tex[8][2];
	u8 pnmtx;     // matrix row index (0-63), from the vertex or the MATINDEX register
	u8 texmtx[8];
};

struct DrawCall {
	u8 primitive;   // Primitive
	u8 vat;         // vertex format 0-7
	u16 count;      // vertices
	u8 inDisplayList;
	const VertexLayout* layout;
	const Vertex* vertices; // decoded, count entries; NULL unless the listener asked for VERTICES_DECODED
	const u8* data;         // the vertices as in the command stream (count * layout->size bytes)
};

// How much of a draw's vertex data the processor works out (Listener::vertexDetail).
enum VertexDetail {
	VERTICES_NONE,    // only the draw itself: cheapest
	VERTICES_ARRAYS,  // also resolve the vertex arrays the draw indexes (checks memory; traces record it)
	VERTICES_DECODED, // also decode every vertex (DrawCall::vertices)
};

struct CopyInfo {
	u32 srcLeft, srcTop, width, height; // EFB rectangle
	u32 dest;                           // destination address as in the command (26-bit)
	u32 destStride;                     // bytes per row of blocks (BP 0x4D << 5)
	u32 control;                        // BP 0x52 value
	bool toXfb;                         // the frame is finished (copy to the external frame buffer)
	bool clear;                         // clear the EFB afterwards
};

// ---------------------------------------------------------------- interfaces for the platform

class Memory {
public:
	virtual ~Memory() { }
	// Returns `size` bytes of game memory at `address` (with the address bits the command kept, see
	// MemKind), or NULL if the address cannot be resolved.
	virtual const u8* read(u32 address, u32 size, MemKind kind) = 0;
};

class Listener {
public:
	virtual ~Listener() { }
	virtual VertexDetail vertexDetail() { return VERTICES_DECODED; } // asked at every draw
	virtual void draw(const DrawCall& call) { (void)call; }
	virtual void copy(const CopyInfo& info) { (void)info; }
	virtual void drawDone() { }                         // BP 0x45: raise the PE finish interrupt
	virtual void token(u16 value, bool interrupt) { (void)value; (void)interrupt; } // BP 0x47/0x48
	virtual void bpWritten(u32 reg, u32 value) { (void)reg; (void)value; }
	virtual void badCommand(u8 opcode, const u8* where, u32 available, bool inDisplayList)
	{
		(void)opcode;
		(void)where;
		(void)available;
		(void)inDisplayList;
	}
	virtual void memoryFailed(u32 address, u32 size, MemKind kind) { (void)address; (void)size; (void)kind; }
};

// ---------------------------------------------------------------- register state

struct State {
	u32 cp[0x100];   // CP registers by their LOAD_CP address
	u32 bp[0x100];   // BP registers (24-bit values)
	u32 xf[0x1100];  // XF memory (0x0000-0x07FF) and registers (0x1000-0x10FF), as 32-bit words
};

struct Stats {
	u64 commands, draws, vertices, displayLists, displayListBytes;
	u64 cpLoads, xfLoads, bpLoads, indexedLoads;
	u64 badCommands, memoryFailures, nestedDisplayLists, xfOutOfRange, vertexBufferOverflows;
	u64 copiesToXfb, copiesToTexture, drawDones, tokens;
	u32 primitives;  // bit (opcode >> 3) & 7 per primitive type seen
};

// ---------------------------------------------------------------- the command processor

class CommandProcessor {
public:
	CommandProcessor();
	~CommandProcessor();

	void setMemory(Memory* memory) { mMemory = memory; }
	void setListener(Listener* listener) { mListener = listener; }

	// The FIFO: bytes in the order the CPU wrote them (values are written big-endian).
	void write8(u32 value);
	void write16(u32 value);
	void write32(u32 value);
	void write(const u8* data, u32 size);

	// True between commands (no partial command buffered).
	bool idle() const { return mPending == 0; }
	// The incomplete command the processor is holding (waiting for the rest of its bytes), if any.
	u32 pendingBytes() const { return mPending; }
	u8 pendingOpcode() const { return mPending ? mPendingBuf[0] : 0; }

	// Position in the FIFO stream: bytes written so far, and (while a command runs, e.g. inside a listener
	// call or a memory read) the offsets of the FIFO command being executed and just past it (for a command
	// inside a display list: the display list call).
	u64 received() const { return mReceived; }
	u64 commandStart() const { return mCmdStart; }
	u64 commandEnd() const { return mCmdEnd; }

	// The register state. (Changing it through the non-const accessor is allowed: cached vertex layouts
	// are recomputed.)
	State& state()
	{
		mLayoutValid = 0;
		return mState;
	}
	const State& state() const { return mState; }
	Stats& stats() { return mStats; }

	// Vertex layout for a vertex format, from the current CP registers.
	void layoutFor(u32 vat, VertexLayout& out) const;
	const VertexLayout& layout(u32 vat)
	{
		vat &= 7;
		if (!(mLayoutValid & (1u << vat))) {
			layoutFor(vat, mLayouts[vat]);
			mLayoutValid |= (u8)(1u << vat);
		}
		return mLayouts[vat];
	}

	// Length of the command at p (avail bytes there), or 0 if more bytes are needed to tell.
	u32 commandLength(const u8* p, u32 avail);

	// Helpers for the platform and listeners.
	static u32 textureSize(u32 format, u32 width, u32 height, u32 levels);
	// Texture map n (0-7): image address (26-bit), width, height, format, mip levels.
	void textureInfo(u32 map, u32& address, u32& width, u32& height, u32& format, u32& levels) const;

private:
	// Executes complete commands from data[0..size) (at stream offset `base` when !inDisplayList).
	// Returns the bytes used; stops early (returning less) if the last command is incomplete and
	// !inDisplayList.
	u32 run(const u8* data, u32 size, bool inDisplayList, u64 base);
	void execute(const u8* p, u32 length, bool inDisplayList);
	void drawCommand(const u8* data, bool inDisplayList);
	void loadCp(u32 reg, u32 value);
	void loadXf(u32 address, u32 count, const u8* values);
	void loadIndexed(u32 op, u32 word);
	void loadBp(u32 word);
	void callDisplayList(u32 address, u32 size);
	void decodeVertices(const VertexLayout& layout, const u8* data, u32 count, bool decode);
	void hold(const u8* bytes, u32 n);

	Memory* mMemory;
	Listener* mListener;
	State mState;
	Stats mStats;
	u32 mBpMask;
	int mDisplayListDepth;

	// partial command from the FIFO
	u8* mPendingBuf;
	u32 mPending;
	u32 mPendingCap;
	u64 mReceived;
	u64 mCmdStart;
	u64 mCmdEnd;

	// vertex layouts by format, computed when first needed after the CP registers change
	VertexLayout mLayouts[8];
	u8 mLayoutValid;

	// decoded vertices of the current draw
	Vertex* mVerts;
	u32 mVertCap;
};

} // namespace gpu

#endif
