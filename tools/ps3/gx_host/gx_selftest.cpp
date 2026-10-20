// PS3 port, host test: drives Nintendo's GX library the way the game does and checks what the port's
// command processor makes of the resulting command stream.

#include "Dolphin/gx.h"
#include "types.h"

#include "../../../platform/ps3/gpu/gx_internal.h"
#include "gx_trace_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern "C" u8* hostArena();
extern "C" unsigned int ps3FramesDrawn(void);
extern "C" void ps3GxStatus(char* buf, unsigned int size);
extern "C" void ps3GxNoteSection(const char* name);

static int sFailures;
#define CHECK(cond)                                                          \
	do {                                                                     \
		if (!(cond)) {                                                       \
			printf("FAILED line %d: %s\n", __LINE__, #cond);                 \
			sFailures++;                                                     \
		}                                                                    \
	} while (0)

struct Recorder : gpu::Listener {
	enum { MAX = 32 };
	struct Draw {
		gpu::u8 prim, vat, inDl;
		gpu::u16 count;
		gpu::Vertex v[8];
	} draws[MAX];
	int n;
	int copies, xfbCopies;
	void draw(const gpu::DrawCall& c) override
	{
		if (n < MAX) {
			Draw& d = draws[n++];
			d.prim  = c.primitive;
			d.vat   = c.vat;
			d.inDl  = c.inDisplayList;
			d.count = c.count;
			memcpy(d.v, c.vertices, sizeof(gpu::Vertex) * (c.count < 8 ? c.count : 8));
		}
	}
	void copy(const gpu::CopyInfo& info) override
	{
		copies++;
		xfbCopies += info.toXfb;
	}
};

static bool near(float a, float b) { return (a - b) < 1e-4f && (b - a) < 1e-4f; }

static u8* sNext;
static void* arenaAlloc(u32 size)
{
	sNext       = (u8*)(((uptr)sNext + 31) & ~(uptr)31);
	void* p     = sNext;
	sNext      += size;
	memset(p, 0, size);
	return p;
}

static void quadDirect()
{
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	GXPosition3f32(-1.0f, 1.0f, -5.0f);
	GXColor4u8(255, 0, 0, 255);
	GXPosition3f32(1.0f, 1.0f, -5.0f);
	GXColor4u8(0, 255, 0, 255);
	GXPosition3f32(1.0f, -1.0f, -5.0f);
	GXColor4u8(0, 0, 255, 255);
	GXPosition3f32(-1.0f, -1.0f, -5.0f);
	GXColor4u8(255, 255, 255, 128);
	GXEnd();
	GXFlush(); // the port interprets commands in batches
	ps3gx::finish(); // (on the graphics thread)
}

// The game's first loading-screen frame (System::Initialise -> startLoading -> loadFunc), call for call:
// DGXGraphics' constructor, then one loop of loadFunc up to its GXDrawDone (test 17 hung there).
static int loadingScreen()
{
	static Recorder rec;
	sNext      = hostArena() + 0x100000;
	void* fifo = arenaAlloc(0x60000);
	GXInit(fifo, 0x60000);
	ps3gx::setRenderer(&rec);
	gpu::CommandProcessor& cp = ps3gx::commandProcessor();

	// DGXGraphics::DGXGraphics (NTSC 480 interlaced, no anti-aliasing)
	static u8 vfilter[7] = { 5, 6, 14, 14, 14, 6, 5 };
	static u8 pattern[12][2];
	for (int i = 0; i < 12; i++) {
		pattern[i][0] = pattern[i][1] = 6;
	}
	GXSetDispCopySrc(0, 0, 640, 480);
	GXSetDispCopyDst(640, 480);
	GXSetDispCopyYScale(1.0f);
	GXSetCopyFilter(GX_FALSE, pattern, GX_TRUE, vfilter);
	GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
	void* xfb = arenaAlloc(640 * 480 * 2);

	// loadFunc
	GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
	GXCopyDisp(xfb, GX_TRUE);
	// System::beginRender
	GXInvalidateTexAll();
	GXInvalidateVtxCache();
	GXColor black = { 0, 0, 0, 0 };
	GXSetCopyClear(black, 0xFFFFFF);
	GXSetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
	GXSetScissor(0, 0, 640, 480);
	GXSetColorUpdate(GX_TRUE);
	for (int pass = 0; pass < 2; pass++) { // useTexture(nullptr, 0) here and in initRender
		GXSetNumTexGens(0);
		GXSetNumTevStages(1);
		GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
		GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
		if (pass == 0) { // DGXGraphics::initRender
			GXSetCullMode(GX_CULL_BACK);
			GXSetCoPlanar(GX_FALSE);
			GXSetColorUpdate(GX_TRUE);
			GXSetDither(GX_FALSE);
			GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_NOOP);
			GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
			GXSetZCompLoc(GX_FALSE);
			GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0);
			GXSetLineWidth(6, GX_TO_ZERO);
			GXSetPointSize(24, GX_TO_ZERO);
		}
	}
	// DGXGraphics::setOrthogonal
	Mtx44 ortho;
	memset(ortho, 0, sizeof(ortho));
	ortho[0][0] = 2.0f / 640.0f;
	ortho[0][3] = -1.0f;
	ortho[1][1] = -2.0f / 480.0f;
	ortho[1][3] = 1.0f;
	ortho[2][2] = 1.0f;
	ortho[3][3] = 1.0f;
	GXSetProjection(ortho, GX_ORTHOGRAPHIC);
	Mtx ident;
	memset(ident, 0, sizeof(ident));
	ident[0][0] = ident[1][1] = ident[2][2] = 1.0f;
	GXLoadPosMtxImm(ident, 0);
	GXSetCurrentMtx(0);
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, 1, GX_DF_NONE, GX_AF_NONE);
	GXSetNumChans(1);
	GXSetZMode(GX_FALSE, GX_LEQUAL, GX_TRUE);
	GXSetScissor(0, 0, 640, 480);
	GXSetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
	GXSetNumTexGens(1);
	GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_TEX0, 0x3c, GX_FALSE, 0x7d);
	GXSetNumTexGens(0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXSetFog(GX_FOG_NONE, 0.0f, 0.0f, 0.0f, 0.0f, black);
	// setColour, fillRectangle
	GXColor shade = { 0, 0, 0, 32 };
	GXSetChanMatColor(GX_COLOR0A0, shade);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_CLR0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetNumTexGens(0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	GXPosition3f32(0.0f, 0.0f, 0.0f);
	GXColor1u32(0x00000020);
	GXPosition3f32(640.0f, 0.0f, 0.0f);
	GXColor1u32(0x00000020);
	GXPosition3f32(640.0f, 480.0f, 0.0f);
	GXColor1u32(0x00000020);
	GXPosition3f32(0.0f, 480.0f, 0.0f);
	GXColor1u32(0x00000020);
	GXEnd();
	// DGXGraphics::doneRender
	GXFlush();
	ps3gx::finish();
	GXDrawDone();
	ps3gx::finish();

	const gpu::Stats& st = cp.stats();
	printf("loading screen: %llu commands, %llu draws, %llu draw dones, %llu bad commands, processor %s\n",
	       (unsigned long long)st.commands, (unsigned long long)st.draws, (unsigned long long)st.drawDones,
	       (unsigned long long)st.badCommands, cp.idle() ? "idle" : "waiting for the rest of a command");
	CHECK(st.drawDones == 1);
	CHECK(st.badCommands == 0);
	CHECK(cp.idle());
	CHECK(rec.xfbCopies == 1);
	CHECK(rec.n >= 1);
	return sFailures ? 1 : 0;
}

int main(int argc, char** argv)
{
	bool loadscreen = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "sync") == 0) {
			ps3gx::forceSynchronous(); // interpret on this thread (otherwise: on the graphics thread)
		} else if (strcmp(argv[i], "loadscreen") == 0) {
			loadscreen = true;
		}
	}
	if (loadscreen) {
		return loadingScreen();
	}
	static Recorder rec; // zero-initialized (and keeps its vtable)
	sNext = hostArena() + 0x100000;

	void* fifo = arenaAlloc(256 * 1024);
	GXInit(fifo, 256 * 1024);
	ps3gx::setRenderer(&rec);
	gpu::CommandProcessor& cp = ps3gx::commandProcessor();

	// 1. immediate-mode quad, direct positions and colors
	quadDirect();
	CHECK(rec.n == 1);
	if (rec.n >= 1) {
		CHECK(rec.draws[0].prim == gpu::PRIM_QUADS && rec.draws[0].count == 4 && !rec.draws[0].inDl);
		CHECK(near(rec.draws[0].v[0].pos[0], -1.0f) && near(rec.draws[0].v[0].pos[2], -5.0f));
		CHECK(near(rec.draws[0].v[2].pos[1], -1.0f));
		CHECK(rec.draws[0].v[1].clr[0][1] == 255 && rec.draws[0].v[3].clr[0][3] == 128);
	}

	// 2. indexed positions (s16, 4 fractional bits) from an array in memory
	s16* positions = (s16*)arenaAlloc(3 * 3 * 2);
	const s16 pos[9] = { 16, 32, -48, 160, 0, 8, -16, -32, 0 };
	for (int i = 0; i < 9; i++) {
		positions[i] = pos[i]; // native big-endian on the PS3; on the host, write big-endian by hand
		u8* b         = (u8*)&positions[i];
		u16 v         = (u16)pos[i];
		b[0]          = (u8)(v >> 8);
		b[1]          = (u8)v;
	}
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
	GXSetVtxAttrFmt(GX_VTXFMT1, GX_VA_POS, GX_POS_XYZ, GX_S16, 4);
	GXSetArray(GX_VA_POS, positions, 6);
	GXBegin(GX_TRIANGLES, GX_VTXFMT1, 3);
	GXWGFifo.u16 = 2;
	GXWGFifo.u16 = 0;
	GXWGFifo.u16 = 1;
	GXEnd();
	GXFlush();
	ps3gx::finish();
	CHECK(rec.n == 2);
	if (rec.n >= 2) {
		CHECK(rec.draws[1].prim == gpu::PRIM_TRIANGLES && rec.draws[1].vat == 1);
		CHECK(near(rec.draws[1].v[0].pos[0], -1.0f) && near(rec.draws[1].v[0].pos[1], -2.0f));
		CHECK(near(rec.draws[1].v[1].pos[0], 1.0f) && near(rec.draws[1].v[1].pos[2], -3.0f));
		CHECK(near(rec.draws[1].v[2].pos[0], 10.0f) && near(rec.draws[1].v[2].pos[2], 0.5f));
	}

	// 3. a display list recorded and called
	void* dl = arenaAlloc(1024);
	GXBeginDisplayList(dl, 1024);
	quadDirect();
	u32 dlSize = GXEndDisplayList();
	CHECK(rec.n == 2); // recording does not draw
	CHECK(dlSize > 0 && (dlSize & 31) == 0);
	GXCallDisplayList(dl, dlSize);
	GXFlush();
	ps3gx::finish();
	CHECK(rec.n == 3);
	if (rec.n >= 3) {
		CHECK(rec.draws[2].inDl && rec.draws[2].count == 4 && near(rec.draws[2].v[1].pos[0], 1.0f));
	}

	// 4. matrices and projection
	Mtx m;
	memset(m, 0, sizeof(m));
	m[0][0] = m[1][1] = m[2][2] = 1.0f;
	m[0][3] = 7.0f;
	m[2][2] = 3.0f;
	GXLoadPosMtxImm(m, GX_PNMTX1);
	GXFlush();
	ps3gx::finish();
	{
		union {
			u32 u;
			f32 f;
		} x;
		x.u = cp.state().xf[GX_PNMTX1 * 4 + 3];
		CHECK(near(x.f, 7.0f));
		x.u = cp.state().xf[GX_PNMTX1 * 4 + 10];
		CHECK(near(x.f, 3.0f));
	}
	Mtx44 proj;
	memset(proj, 0, sizeof(proj));
	proj[0][0] = 1.3f;
	proj[1][1] = 1.7f;
	proj[2][2] = -1.0f;
	proj[2][3] = -2.0f;
	proj[3][2] = -1.0f;
	GXSetProjection(proj, GX_PERSPECTIVE);
	GXFlush();
	ps3gx::finish();
	CHECK(cp.state().xf[0x1026] == 0); // projection type: perspective

	// 5. a texture used by TEV stage 0
	u8* tex = (u8*)arenaAlloc(64 * 64 * 4);
	GXTexObj obj;
	GXInitTexObj(&obj, tex, 64, 64, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	GXLoadTexObj(&obj, GX_TEXMAP0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GXSetNumTexGens(1);
	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_POS, GX_POS_XY, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT2, GX_VA_TEX0, GX_TEX_ST, GX_U16, 8);
	GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT2, 3);
	GXPosition2f32(0.0f, 0.0f);
	GXTexCoord2u16(256, 128);
	GXPosition2f32(1.0f, 0.0f);
	GXTexCoord2u16(0, 0);
	GXPosition2f32(0.0f, 1.0f);
	GXTexCoord2u16(512, 0);
	GXEnd();
	GXFlush();
	ps3gx::finish();
	CHECK(rec.n == 4);
	if (rec.n >= 4) {
		CHECK(rec.draws[3].prim == gpu::PRIM_TRI_STRIP);
		CHECK(near(rec.draws[3].v[0].tex[0][0], 1.0f) && near(rec.draws[3].v[0].tex[0][1], 0.5f));
		CHECK(near(rec.draws[3].v[2].tex[0][0], 2.0f));
	}
	{
		u32 address, w, h, fmt, levels;
		cp.textureInfo(0, address, w, h, fmt, levels);
		CHECK(w == 64 && h == 64 && fmt == gpu::TF_RGBA8 && levels == 1);
		CHECK(address == ((u32)(uptr)tex & 0x3FFFFE0));
	}

	// 6. the end of a frame, draw done, draw sync token
	void* xfb = arenaAlloc(640 * 480 * 2);
	GXSetDispCopySrc(0, 0, 640, 480);
	GXSetDispCopyDst(640, 480);
	GXCopyDisp(xfb, GX_TRUE);
	GXFlush();
	ps3gx::finish();
	CHECK(rec.xfbCopies == 1);
	CHECK(ps3FramesDrawn() == 1);
	GXDrawDone();
	ps3gx::finish();
	GXSetDrawSync(0x1234);
	ps3gx::finish();
	CHECK(GXReadDrawSync() == 0x1234);

	// 7. frame traces (with GX_TRACE_DIR set): schedule one, draw frames until it is written
	if (getenv("GX_TRACE_DIR")) {
		ps3GxNoteSection("Nintendo logo"); // recorded 30 frames later
		for (int f = 0; f < 40; f++) {
			quadDirect();
			GXClearVtxDesc();
			GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
			GXBegin(GX_TRIANGLES, GX_VTXFMT1, 3);
			GXWGFifo.u16 = 2;
			GXWGFifo.u16 = 0;
			GXWGFifo.u16 = 1;
			GXEnd();
			GXCallDisplayList(dl, dlSize);
			GXLoadTexObj(&obj, GX_TEXMAP0);
			GXClearVtxDesc();
			GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
			GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
			GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT2, 3);
			GXPosition2f32(0.0f, 0.0f);
			GXTexCoord2u16(256, 128);
			GXPosition2f32(1.0f, 0.0f);
			GXTexCoord2u16(0, 0);
			GXPosition2f32(0.0f, 1.0f);
			GXTexCoord2u16(512, 0);
			GXEnd();
			GXCopyDisp(xfb, GX_TRUE);
			GXDrawDone();
			ps3gx::finish();
		}
		for (int i = 0; i < 100 && !ps3gx::traceRecording(); i++) {
			char st[96];
			ps3gx::traceStatus(st, sizeof(st));
			if (strstr(st, "1 frames recorded")) {
				break;
			}
		}
		usleep(300 * 1000); // the writer thread

		// The recording holds exactly one whole frame: replayed, it has the frame's four draws and its
		// copy to the display, and nothing else.
		char path[512];
		snprintf(path, sizeof(path), "%s/gx_trace_01_nintendo_logo.bin", getenv("GX_TRACE_DIR"));
		TraceFile trace;
		CHECK(trace.load(path));
		CHECK(trace.version == 3 && !trace.incomplete);
		struct Count : gpu::Listener {
			int draws, copies;
			void draw(const gpu::DrawCall& c) override { draws++; (void)c; }
			void copy(const gpu::CopyInfo& c) override { copies += c.toXfb; }
		} count;
		count.draws = count.copies = 0;
		gpu::CommandProcessor replay;
		TraceMemory mem(trace);
		replay.setMemory(&mem);
		replay.setListener(&count);
		replayTrace(trace, replay, mem);
		printf("trace: frame %u, %u FIFO bytes (frame: %u), %u memory blocks; replay: %d draws, %d copies, %llu memory failures\n",
		       trace.frame, trace.fifoBytes, trace.fifoLimit, trace.memEvents, count.draws, count.copies,
		       (unsigned long long)replay.stats().memoryFailures);
		CHECK(count.draws == 4 && count.copies == 1);
		CHECK(replay.stats().memoryFailures == 0 && replay.stats().badCommands == 0);
		CHECK(replay.idle());
	}

	// 9. many writes of every size: around the ring several times, across many publishing points
	{
		const u64 bp0    = cp.stats().bpLoads;
		const u64 draws0 = cp.stats().draws;
		const u32 N      = 300000;
		GXColor last     = { 0, 0, 0, 0 };
		u32 quads        = 0;
		for (u32 i = 0; i < N; i++) {
			GXColor c = { (u8)i, (u8)(i >> 8), (u8)(i >> 16), (u8)(i * 7) };
			GXSetTevColor(GX_TEVREG1, c);
			last = c;
			if (i % 997 == 0) {
				quadDirect(); // (8-, 16- and 32-bit writes: the stream's alignment changes)
				quads++;
			}
		}
		GXFlush();
		ps3gx::finish();
		CHECK(cp.stats().bpLoads - bp0 == (u64)N * 4);
		CHECK(cp.stats().draws - draws0 == quads);
		const u32 ra = cp.state().bp[0xE0 + GX_TEVREG1 * 2], bg = cp.state().bp[0xE1 + GX_TEVREG1 * 2];
		CHECK((ra & 0x7FF) == last.r && ((ra >> 12) & 0x7FF) == last.a);
		CHECK((bg & 0x7FF) == last.b && ((bg >> 12) & 0x7FF) == last.g);
		printf("stress: %u colors and %u quads written, %llu BP loads interpreted\n", N, quads, (unsigned long long)(cp.stats().bpLoads - bp0));
	}

	const gpu::Stats& st = cp.stats();
	CHECK(st.badCommands == 0);
	CHECK(st.memoryFailures == 0);
	CHECK(st.drawDones >= 1);
	CHECK(cp.idle());

	char status[800];
	ps3GxStatus(status, sizeof(status));
	printf("%s\n", status);
	printf("%llu commands, %llu draws, %llu vertices, %llu display lists (%llu bytes), %llu CP / %llu XF / %llu BP loads\n",
	       (unsigned long long)st.commands, (unsigned long long)st.draws, (unsigned long long)st.vertices,
	       (unsigned long long)st.displayLists, (unsigned long long)st.displayListBytes, (unsigned long long)st.cpLoads,
	       (unsigned long long)st.xfLoads, (unsigned long long)st.bpLoads);
	if (sFailures) {
		printf("%d checks FAILED\n", sFailures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
