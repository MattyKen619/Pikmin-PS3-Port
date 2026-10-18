// PS3 port: the renderer's Device on the RSX (see device_rsx.h).
//
// Set-up and state follow PSL1GHT's rsxtest sample (known to work on the console): linear A8R8G8B8 render
// targets with a Z24S8 depth buffer, depth clamping instead of near/far culling, smooth shading, textures
// as linear A8R8G8B8 with the identity channel mapping.

#include "device_rsx.h"

#include "../backend/ps3_backend.h"

#include <ppu-types.h>
#include <stdint.h>
#include <rsx/rsx.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "passthrough_vpo.h" // (generated from passthrough.vp at build time)

#define CTX ((gcmContextData*)mCtx)

namespace gfx {

enum {
	FENCE_LABEL = 254,
	READ_LABEL  = 253,
	EFB_PITCH   = EFB_WIDTH * 4,
};

static volatile int sStopRequest;
static volatile int sStopped;
static RsxDevice* sDevice;

static void logf(const char* fmt, ...)
{
	char buf[512];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);
	ps3b_log_write(buf);
}

static inline u32 offsetOf(const void* p)
{
	u32 o = 0;
	rsxAddressToOffset(p, &o);
	return o;
}

RsxDevice::RsxDevice()
    : mCtx(NULL)
    , mScratch(NULL)
    , mScratchSize(0)
    , mDispWidth(0)
    , mDispHeight(0)
    , mDispPitch(0)
    , mDispNext(0)
    , mFlipped(false)
    , mDispDepthOffset(0)
    , mFlips(0)
    , mEfbColorMem(NULL)
    , mEfbColorOffset(0)
    , mEfbDepthOffset(0)
    , mTargetMem(NULL)
    , mTargetPitch(0)
    , mTargetWidth(0)
    , mTargetHeight(0)
    , mStateValid(false)
    , mBoundTexCoords(8)
    , mInvalidateTextures(true)
    , mReadLabelValue(1)
    , mLocalUsed(0)
{
	memset(&mState, 0, sizeof(mState));
	memset(&mEfbTexture, 0, sizeof(mEfbTexture));
}

// ================================================================== exit

void RsxDevice::checkStop()
{
	if (sStopRequest) {
		sStopped = 1;
		for (;;) {
			ps3b_sleep_us(1000 * 1000); // (the process is about to end)
		}
	}
}

// The graphics thread stops at its next RSX call; whatever the RSX is still doing ends with the process
// (the exit thread must not write to the command buffer the graphics thread may be writing).
void RsxDevice::stopForExit()
{
	if (!sDevice) {
		return;
	}
	sStopRequest = 1;
	for (int i = 0; i < 20 && !sStopped; i++) {
		ps3b_sleep_us(1000);
	}
}

// ================================================================== set-up

bool RsxDevice::init()
{
	Ps3bDisplay d;
	if (ps3b_console_take_display(&d) != 0) {
		logf("[rsx] the display is not available (the text console could not set it up)\n");
		return false;
	}
	mCtx         = d.context;
	mDispWidth   = d.width;
	mDispHeight  = d.height;
	mDispPitch   = d.pitch;
	mDispOffset[0] = d.offsets[0];
	mDispOffset[1] = d.offsets[1];
	mDispBuffer[0] = d.buffers[0];
	mDispBuffer[1] = d.buffers[1];
	mDispNext    = d.next;
	mFlipped     = d.flipped != 0;
	mScratch     = (u8*)d.host + d.commandSize;
	mScratchSize = (d.hostSize - d.commandSize) & ~127u;

	// local memory: the frame buffer, its depth buffer, and a depth buffer for drawing to the display
	mEfbColorMem   = (u32*)rsxMemalign(128, EFB_PITCH * EFB_HEIGHT); // (also read as a texture)
	void* efbDepth = rsxMemalign(64, EFB_PITCH * EFB_HEIGHT);
	u32 dispDepthPitch = mDispWidth * 4;
	void* dispDepth    = rsxMemalign(64, dispDepthPitch * mDispHeight);
	if (!mEfbColorMem || !efbDepth || !dispDepth) {
		logf("[rsx] FAILED to allocate the frame buffers in RSX memory\n");
		return false;
	}
	mLocalUsed += 2 * EFB_PITCH * EFB_HEIGHT + dispDepthPitch * mDispHeight;
	mEfbColorOffset  = offsetOf(mEfbColorMem);
	mEfbDepthOffset  = offsetOf(efbDepth);
	mDispDepthOffset = offsetOf(dispDepth);
	mEfbTexture.width  = EFB_WIDTH;
	mEfbTexture.height = EFB_HEIGHT;
	mEfbTexture.pitch  = EFB_PITCH;
	mEfbTexture.texels = mEfbColorMem;
	mEfbTexture.bytes  = 0;
	mEfbTexture.impl   = (void*)(uintptr_t)mEfbColorOffset;
	*(vu32*)gcmGetLabelAddress(FENCE_LABEL) = 0;
	*(vu32*)gcmGetLabelAddress(READ_LABEL)  = 0;

	gcmContextData* ctx = CTX;
	rsxSetColorMaskMrt(ctx, 0);
	rsxSetZMinMaxControl(ctx, GCM_FALSE, GCM_TRUE, GCM_FALSE);
	rsxSetShadeModel(ctx, GCM_SHADE_MODEL_SMOOTH);
	rsxSetFrontFace(ctx, GCM_FRONTFACE_CCW);
	rsxSetCullFaceEnable(ctx, GCM_FALSE);
	rsxSetFrontPolygonMode(ctx, GCM_POLYGON_MODE_FILL);
	rsxSetBackPolygonMode(ctx, GCM_POLYGON_MODE_FILL);
	rsxSetDitherEnable(ctx, GCM_FALSE);
	rsxSetAlphaTestEnable(ctx, GCM_FALSE);
	rsxSetStencilTestEnable(ctx, GCM_FALSE);
	rsxSetTwoSidedStencilTestEnable(ctx, GCM_FALSE);
	rsxSetPolygonOffsetFillEnable(ctx, GCM_FALSE);
	rsxSetDepthBoundsTestEnable(ctx, GCM_FALSE);
	rsxSetLogicOpEnable(ctx, GCM_FALSE);
	rsxSetBlendEnable(ctx, GCM_FALSE);
	rsxSetBlendEnableMrt(ctx, GCM_FALSE, GCM_FALSE, GCM_FALSE);
	rsxSetRestartIndexEnable(ctx, GCM_FALSE);
	rsxSetUserClipPlaneControl(ctx, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
	                           GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
	for (int i = 0; i < 16; i++) {
		rsxBindVertexArrayAttrib(ctx, (u8)i, 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
		rsxTextureControl(ctx, (u8)i, GCM_FALSE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	}
	mBoundTexCoords = 0;
	const rsxVertexProgram* vp = (const rsxVertexProgram*)kPassthroughVpo;
	void* vpUcode;
	u32 vpSize;
	rsxVertexProgramGetUCode(vp, &vpUcode, &vpSize);
	rsxLoadVertexProgram(ctx, vp, vpUcode);
	setTarget(NULL);
	rsxFlushBuffer(ctx);
	sDevice = this;
	ps3b_console_set_display_owner_stop(stopForExit);
	logf("[rsx] took the display over: %ux%u, %u MB of draw memory, frame buffer at 0x%08x\n", mDispWidth, mDispHeight,
	     mScratchSize >> 20, mEfbColorOffset);
	return true;
}

// ================================================================== fences

void RsxDevice::writeFence(u32 value)
{
	checkStop();
	rsxSetWriteBackendLabel(CTX, FENCE_LABEL, value);
}

u32 RsxDevice::fenceReached() { return *(vu32*)gcmGetLabelAddress(FENCE_LABEL); }

void RsxDevice::kick()
{
	checkStop();
	rsxFlushBuffer(CTX);
}

void RsxDevice::pause()
{
	checkStop();
	ps3b_sleep_us(30);
}

// Waits for everything submitted so far (value 0: a new value).
bool RsxDevice::waitLabel(u32 value, u32 timeoutUs)
{
	if (!value) {
		value = ++mReadLabelValue;
	}
	rsxSetWriteBackendLabel(CTX, READ_LABEL, value);
	rsxFlushBuffer(CTX);
	for (u32 t = 0; t < timeoutUs; t += 50) {
		if (*(vu32*)gcmGetLabelAddress(READ_LABEL) == value) {
			return true;
		}
		ps3b_sleep_us(50);
	}
	return false;
}

// ================================================================== textures

Texture* RsxDevice::createTexture(u32 width, u32 height)
{
	const u32 pitch = (width * 4 + 63) & ~63u;
	const u32 bytes = pitch * height;
	void* mem       = rsxMemalign(128, bytes);
	if (!mem) {
		return NULL;
	}
	Texture* t = create<Texture>();
	t->width   = width;
	t->height  = height;
	t->pitch   = pitch;
	t->texels  = (u32*)mem;
	t->bytes   = bytes;
	t->impl    = (void*)(uintptr_t)offsetOf(mem);
	mLocalUsed += bytes;
	return t;
}

void RsxDevice::destroyTexture(Texture* t)
{
	mLocalUsed -= t->bytes;
	rsxFree(t->texels);
	destroy(t);
}

// ================================================================== targets

void RsxDevice::surface(u32 colorOffset, u32 colorPitch, u32 width, u32 height, u32 depthOffset, u32 depthPitch, bool argb)
{
	gcmContextData* ctx = CTX;
	gcmSurface sf;
	memset(&sf, 0, sizeof(sf));
	sf.colorFormat      = argb ? GCM_SURFACE_A8R8G8B8 : GCM_SURFACE_X8R8G8B8;
	sf.colorTarget      = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0]   = colorOffset;
	sf.colorPitch[0]    = colorPitch;
	for (int i = 1; i < 4; i++) {
		sf.colorLocation[i] = GCM_LOCATION_RSX;
		sf.colorOffset[i]   = 0;
		sf.colorPitch[i]    = 64;
	}
	sf.depthFormat   = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset   = depthOffset;
	sf.depthPitch    = depthPitch;
	sf.type          = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias     = GCM_SURFACE_CENTER_1;
	sf.width         = (u16)width;
	sf.height        = (u16)height;
	sf.x             = 0;
	sf.y             = 0;
	rsxSetSurface(ctx, &sf);
	for (int i = 0; i < 8; i++) {
		rsxSetViewportClip(ctx, (u8)i, (u16)width, (u16)height);
	}
	mTargetWidth  = width;
	mTargetHeight = height;
	mStateValid   = false;
}

void RsxDevice::setTarget(const Texture* t)
{
	checkStop();
	if (!t) {
		surface(mEfbColorOffset, EFB_PITCH, EFB_WIDTH, EFB_HEIGHT, mEfbDepthOffset, EFB_PITCH, true);
		mTargetMem   = mEfbColorMem;
		mTargetPitch = EFB_PITCH;
	} else {
		surface((u32)(uintptr_t)t->impl, t->pitch, t->width, t->height, mEfbDepthOffset, EFB_PITCH, true);
		mTargetMem   = t->texels;
		mTargetPitch = t->pitch;
	}
}

bool RsxDevice::beginDisplay(u32* width, u32* height)
{
	checkStop();
	surface(mDispOffset[mDispNext], mDispPitch, mDispWidth, mDispHeight, mDispDepthOffset, mDispWidth * 4, false);
	mTargetMem   = mDispBuffer[mDispNext];
	mTargetPitch = mDispPitch;
	*width       = mDispWidth;
	*height      = mDispHeight;
	return true;
}

void RsxDevice::endDisplay()
{
	checkStop();
	gcmContextData* ctx = CTX;
	if (mFlipped) {
		// (the previous flip must have happened before another is queued)
		for (int spins = 0; gcmGetFlipStatus() != 0 && spins < 10000; spins++) {
			ps3b_sleep_us(100);
		}
	}
	gcmResetFlipStatus();
	mFlipped = true;
	gcmSetFlip(ctx, (u8)mDispNext);
	rsxFlushBuffer(ctx);
	gcmSetWaitFlip(ctx);
	mDispNext ^= 1;
	mFlips++;
}

// ================================================================== state

static u16 blendFactor(u8 f)
{
	switch (f) {
	case BLEND_ZERO: return GCM_ZERO;
	case BLEND_ONE: return GCM_ONE;
	case BLEND_SRC_COLOR: return GCM_SRC_COLOR;
	case BLEND_INV_SRC_COLOR: return GCM_ONE_MINUS_SRC_COLOR;
	case BLEND_DST_COLOR: return GCM_DST_COLOR;
	case BLEND_INV_DST_COLOR: return GCM_ONE_MINUS_DST_COLOR;
	case BLEND_SRC_ALPHA: return GCM_SRC_ALPHA;
	case BLEND_INV_SRC_ALPHA: return GCM_ONE_MINUS_SRC_ALPHA;
	case BLEND_DST_ALPHA: return GCM_DST_ALPHA;
	default: return GCM_ONE_MINUS_DST_ALPHA;
	}
}

void RsxDevice::applyState(const RenderState& s, bool all)
{
	gcmContextData* ctx  = CTX;
	const RenderState& o = mState;
	if (all || s.blend != o.blend || s.srcFactor != o.srcFactor || s.dstFactor != o.dstFactor) {
		rsxSetBlendEnable(ctx, s.blend ? GCM_TRUE : GCM_FALSE);
		if (s.blend) {
			u16 sf = blendFactor(s.srcFactor), df = blendFactor(s.dstFactor);
			rsxSetBlendFunc(ctx, sf, df, sf, df);
			u16 eq = s.blend == 2 ? GCM_FUNC_REVERSE_SUBTRACT : GCM_FUNC_ADD;
			rsxSetBlendEquation(ctx, eq, eq);
		}
	}
	if (all || s.logicEnable != o.logicEnable || s.logicOp != o.logicOp || s.blend != o.blend) {
		bool on = s.logicEnable && !s.blend;
		rsxSetLogicOpEnable(ctx, on ? GCM_TRUE : GCM_FALSE);
		if (on) {
			rsxSetLogicOp(ctx, GCM_CLEAR + (s.logicOp & 15));
		}
	}
	if (all || s.depthTest != o.depthTest || s.depthFunc != o.depthFunc || s.depthWrite != o.depthWrite) {
		rsxSetDepthTestEnable(ctx, s.depthTest ? GCM_TRUE : GCM_FALSE);
		rsxSetDepthFunc(ctx, GCM_NEVER + (s.depthFunc & 7));
		rsxSetDepthWriteEnable(ctx, s.depthWrite ? GCM_TRUE : GCM_FALSE);
	}
	if (all || s.colorWrite != o.colorWrite || s.alphaWrite != o.alphaWrite) {
		u32 mask = (s.colorWrite ? (GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B) : 0) | (s.alphaWrite ? GCM_COLOR_MASK_A : 0);
		rsxSetColorMask(ctx, mask);
	}
	if (all || s.scissor[0] != o.scissor[0] || s.scissor[1] != o.scissor[1] || s.scissor[2] != o.scissor[2] || s.scissor[3] != o.scissor[3]) {
		rsxSetScissor(ctx, s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
	}
	if (all || memcmp(s.viewportScale, o.viewportScale, sizeof(s.viewportScale)) != 0
	    || memcmp(s.viewportOffset, o.viewportOffset, sizeof(s.viewportOffset)) != 0) {
		f32 scale[4]  = { s.viewportScale[0], s.viewportScale[1], s.viewportScale[2], 0.0f };
		f32 offset[4] = { s.viewportOffset[0], s.viewportOffset[1], s.viewportOffset[2], 0.0f };
		rsxSetViewport(ctx, 0, 0, (u16)mTargetWidth, (u16)mTargetHeight, 0.0f, 1.0f, scale, offset);
	}
}

void RsxDevice::setState(const RenderState& s)
{
	checkStop();
	applyState(s, !mStateValid);
	mState      = s;
	mStateValid = true;
}

void RsxDevice::setProgram(const void* ucode, const FragmentProgram& fp)
{
	checkStop();
	rsxFragmentProgram h;
	memset(&h, 0, sizeof(h));
	h.num_regs   = (u16)fp.numRegs;
	h.fp_control = fp.control;
	h.texcoords  = fp.texcoords;
	h.texcoord2D = fp.texcoord2D;
	h.texcoord3D = fp.texcoord3D;
	rsxLoadFragmentProgramLocation(CTX, &h, offsetOf(ucode), GCM_LOCATION_CELL);
}

void RsxDevice::setTexture(u32 unit, const Texture* t, const Sampler& s)
{
	checkStop();
	gcmContextData* ctx = CTX;
	if (!t) {
		rsxTextureControl(ctx, (u8)unit, GCM_FALSE, 0, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
		return;
	}
	gcmTexture tex;
	memset(&tex, 0, sizeof(tex));
	tex.format    = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	tex.mipmap    = 1;
	tex.dimension = GCM_TEXTURE_DIMS_2D;
	tex.cubemap   = GCM_FALSE;
	tex.remap     = ((GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_B_SHIFT) | (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_G_SHIFT)
                 | (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_R_SHIFT) | (GCM_TEXTURE_REMAP_TYPE_REMAP << GCM_TEXTURE_REMAP_TYPE_A_SHIFT)
                 | (GCM_TEXTURE_REMAP_COLOR_B << GCM_TEXTURE_REMAP_COLOR_B_SHIFT) | (GCM_TEXTURE_REMAP_COLOR_G << GCM_TEXTURE_REMAP_COLOR_G_SHIFT)
                 | (GCM_TEXTURE_REMAP_COLOR_R << GCM_TEXTURE_REMAP_COLOR_R_SHIFT) | (GCM_TEXTURE_REMAP_COLOR_A << GCM_TEXTURE_REMAP_COLOR_A_SHIFT));
	tex.width    = (u16)t->width;
	tex.height   = (u16)t->height;
	tex.depth    = 1;
	tex.location = GCM_LOCATION_RSX;
	tex.pitch    = t->pitch;
	tex.offset   = (u32)(uintptr_t)t->impl;
	static const u8 kWrap[3] = { GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_REPEAT, GCM_TEXTURE_MIRRORED_REPEAT };
	rsxLoadTexture(ctx, (u8)unit, &tex);
	rsxTextureControl(ctx, (u8)unit, GCM_TRUE, 0 << 8, 12 << 8, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(ctx, (u8)unit, 0, s.linearMin ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST,
	                 s.linearMag ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(ctx, (u8)unit, kWrap[s.wrapS < 3 ? s.wrapS : 0], kWrap[s.wrapT < 3 ? s.wrapT : 0],
	                   GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

void RsxDevice::setVertices(const void* base, u32 stride, u32 texCoords)
{
	checkStop();
	gcmContextData* ctx = CTX;
	const u32 o         = offsetOf(base);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_POS, 0, o, (u8)stride, 4, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR0, 0, o + 16, (u8)stride, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_CELL);
	rsxBindVertexArrayAttrib(ctx, GCM_VERTEX_ATTRIB_COLOR1, 0, o + 20, (u8)stride, 4, GCM_VERTEX_DATA_TYPE_U8, GCM_LOCATION_CELL);
	for (u32 t = 0; t < texCoords; t++) {
		rsxBindVertexArrayAttrib(ctx, (u8)(GCM_VERTEX_ATTRIB_TEX0 + t), 0, o + 24 + 12 * t, (u8)stride, 3, GCM_VERTEX_DATA_TYPE_F32,
		                         GCM_LOCATION_CELL);
	}
	for (u32 t = texCoords; t < mBoundTexCoords; t++) {
		rsxBindVertexArrayAttrib(ctx, (u8)(GCM_VERTEX_ATTRIB_TEX0 + t), 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	}
	mBoundTexCoords = texCoords;
}

void RsxDevice::drawTriangles(const unsigned short* indices, u32 count)
{
	checkStop();
	gcmContextData* ctx = CTX;
	if (mInvalidateTextures) {
		// (texels written by the CPU, or drawn by the RSX itself: let earlier drawing finish, then drop the
		// texture cache)
		rsxSetWaitForIdle(ctx);
		rsxInvalidateTextureCache(ctx, GCM_INVALIDATE_TEXTURE);
		mInvalidateTextures = false;
	}
	rsxDrawInlineIndexArray16(ctx, GCM_TYPE_TRIANGLES, 0, count, indices);
}

void RsxDevice::clear(const u16 rect[4], u32 argb, u32 depth, bool color, bool alpha, bool depthToo)
{
	checkStop();
	gcmContextData* ctx = CTX;
	u32 mask = (color ? (GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B) : 0) | (alpha ? GCM_CLEAR_A : 0) | (depthToo ? (GCM_CLEAR_Z | GCM_CLEAR_S) : 0);
	if (!mask) {
		return;
	}
	// (the clear obeys the scissor and the write masks: set them for the clear, then back)
	rsxSetScissor(ctx, rect[0], rect[1], rect[2], rect[3]);
	rsxSetColorMask(ctx, (color ? (GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B) : 0) | (alpha ? GCM_COLOR_MASK_A : 0));
	rsxSetDepthWriteEnable(ctx, depthToo ? GCM_TRUE : GCM_FALSE);
	rsxSetClearColor(ctx, argb);
	rsxSetClearDepthStencil(ctx, (depth & 0xFFFFFF) << 8);
	rsxClearSurface(ctx, mask);
	if (mStateValid) {
		const RenderState& s = mState;
		rsxSetScissor(ctx, s.scissor[0], s.scissor[1], s.scissor[2], s.scissor[3]);
		rsxSetColorMask(ctx, (s.colorWrite ? (GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B) : 0) | (s.alphaWrite ? GCM_COLOR_MASK_A : 0));
		rsxSetDepthWriteEnable(ctx, s.depthWrite ? GCM_TRUE : GCM_FALSE);
	}
}

bool RsxDevice::readTarget(u32* out, u32 x, u32 y, u32 width, u32 height)
{
	checkStop();
	if (!waitLabel(0, 2000 * 1000)) {
		return false;
	}
	for (u32 j = 0; j < height; j++) {
		const u32* row = (const u32*)((const u8*)mTargetMem + (y + j) * mTargetPitch) + x;
		for (u32 i = 0; i < width; i++) {
			out[j * width + i] = (x + i < mTargetWidth && y + j < mTargetHeight) ? row[i] : 0;
		}
	}
	return true;
}

// ================================================================== self-test

void RsxDevice::selfTest()
{
	char report[512];
	int n = 0;
	bool allOk = true;
#define NOTE(...) (n += snprintf(report + n, n < (int)sizeof(report) ? sizeof(report) - n : 0, __VA_ARGS__))

	// programs
	const char* error;
	FragmentProgram colorFp, texFp, constFp;
	bool fpOk = assembleFragmentProgram("!!FP2.0\nMOVR o[COLR], f[COL0];\nEND\n", colorFp, &error)
	         && assembleFragmentProgram("!!FP2.0\nTEXR R0, f[TEX0], texture[0], 2D;\nMOVR o[COLR], R0;\nEND\n", texFp, &error)
	         && assembleFragmentProgram("!!FP2.0\n#var float4 k :  : c[0] : 1 : 1\nMOVR R0, c[0];\nMOVR o[COLR], R0;\nEND\n", constFp, &error);
	if (!fpOk) {
		logf("[rsx] self-test: FAILED to assemble a test program (%s)\n", error);
		return;
	}
	u8* mem = mScratch; // (nothing else uses the draw memory yet)
	u32* fpColor = (u32*)mem;
	u32* fpTex   = (u32*)(mem + 1024);
	u32* fpConst = (u32*)(mem + 2048);
	memcpy(fpColor, &colorFp.ucode[0], colorFp.ucode.size() * 4);
	memcpy(fpTex, &texFp.ucode[0], texFp.ucode.size() * 4);
	memcpy(fpConst, &constFp.ucode[0], constFp.ucode.size() * 4);
	const float kv[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
	for (size_t i = 0; i < constFp.relocs.size(); i++) {
		for (int k = 0; k < 4; k++) {
			fpConst[constFp.relocs[i].slot * 4 + k] = fpConstantWord(kv[k]);
		}
	}
	u8* verts = mem + 4096;

	setTarget(NULL);
	RenderState rs;
	memset(&rs, 0, sizeof(rs));
	rs.colorWrite = rs.alphaWrite = 1;
	rs.scissor[2] = EFB_WIDTH, rs.scissor[3] = EFB_HEIGHT;
	rs.viewportScale[0] = EFB_WIDTH * 0.5f, rs.viewportScale[1] = EFB_HEIGHT * -0.5f, rs.viewportScale[2] = 0.5f;
	rs.viewportOffset[0] = EFB_WIDTH * 0.5f, rs.viewportOffset[1] = EFB_HEIGHT * 0.5f, rs.viewportOffset[2] = 0.5f;
	setState(rs);

	// a quad from pixel (x0, y0) to (x1, y1) at depth z (0-1), with a color and texture coordinates 0-1
	int quads = 0;
	auto quad = [&](float x0, float y0, float x1, float y1, float z, u32 rgba) {
		u8* v              = verts + quads * 4 * 36;
		const float xs[4]  = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
		const float us[4]  = { 0, 1, 1, 0 }, vs[4] = { 0, 0, 1, 1 };
		for (int i = 0; i < 4; i++) {
			float* f = (float*)(v + i * 36);
			f[0]     = xs[i] / EFB_WIDTH * 2.0f - 1.0f;
			f[1]     = 1.0f - ys[i] / EFB_HEIGHT * 2.0f;
			f[2]     = z * 2.0f - 1.0f;
			f[3]     = 1.0f;
			u8* c    = v + i * 36 + 16;
			c[0] = (u8)(rgba >> 24), c[1] = (u8)(rgba >> 16), c[2] = (u8)(rgba >> 8), c[3] = (u8)rgba;
			c[4] = c[0], c[5] = c[1], c[6] = c[2], c[7] = c[3];
			float* t = (float*)(v + i * 36 + 24);
			t[0] = us[i], t[1] = vs[i], t[2] = 1.0f;
		}
		setVertices(v, 36, 1);
		static const unsigned short idx[6] = { 0, 1, 2, 0, 2, 3 };
		drawTriangles(idx, 6);
		quads++;
	};
	auto pixel = [&](u32 x, u32 y) -> u32 {
		u32 p = 0;
		if (!readTarget(&p, x, y, 1, 1)) {
			return 0xDEADBEEF;
		}
		return p;
	};

	// 1. clear
	const u16 all[4] = { 0, 0, EFB_WIDTH, EFB_HEIGHT };
	clear(all, 0xFF102030, 0xFFFFFF, true, true, true);
	u32 p = pixel(5, 5);
	bool ok = p == 0xFF102030;
	allOk &= ok;
	NOTE("clear %s", ok ? "ok" : "WRONG");
	if (!ok) NOTE(" (%08x)", p);

	// 2. a colored quad near the top left (vertex colors, orientation)
	setProgram(fpColor, colorFp);
	quad(100, 50, 200, 100, 0.5f, 0xFF0000FF);
	u32 in = pixel(150, 75), mirrored = pixel(150, EFB_HEIGHT - 75);
	ok = in == 0xFFFF0000 && mirrored == 0xFF102030;
	allOk &= ok;
	NOTE(", color %s", ok ? "ok" : "WRONG");
	if (!ok) NOTE(" (%08x, %08x)", in, mirrored);

	// 3. texture: 2x2 red, green / blue, white
	Texture* t = createTexture(2, 2);
	if (t) {
		t->texels[0]              = 0xFFFF0000;
		t->texels[1]              = 0xFF00FF00;
		t->texels[t->pitch / 4]     = 0xFF0000FF;
		t->texels[t->pitch / 4 + 1] = 0xFFFFFFFF;
		texturesChanged();
		Sampler sm;
		sm.wrapS = sm.wrapT = WRAP_CLAMP;
		sm.linearMin = sm.linearMag = 0;
		setTexture(0, t, sm);
		setProgram(fpTex, texFp);
		quad(300, 50, 400, 150, 0.5f, 0xFFFFFFFF);
		u32 q[4] = { pixel(325, 75), pixel(375, 75), pixel(325, 125), pixel(375, 125) };
		ok = q[0] == 0xFFFF0000 && q[1] == 0xFF00FF00 && q[2] == 0xFF0000FF && q[3] == 0xFFFFFFFF;
		allOk &= ok;
		NOTE(", texture %s", ok ? "ok" : "WRONG");
		if (!ok) NOTE(" (%08x %08x %08x %08x)", q[0], q[1], q[2], q[3]);
	}

	// 4. depth: near red, then far green (hidden), then nearer blue
	setProgram(fpColor, colorFp);
	RenderState ds = rs;
	ds.depthTest = 1, ds.depthWrite = 1, ds.depthFunc = 1; // LESS
	setState(ds);
	quad(100, 200, 200, 250, 0.25f, 0xFF0000FF);
	quad(100, 200, 200, 250, 0.75f, 0x00FF00FF);
	u32 d1 = pixel(150, 225);
	quad(100, 200, 200, 250, 0.10f, 0x0000FFFF);
	u32 d2 = pixel(150, 225);
	ok = d1 == 0xFFFF0000 && d2 == 0xFF0000FF;
	allOk &= ok;
	NOTE(", depth %s", ok ? "ok" : "WRONG");
	if (!ok) NOTE(" (%08x, %08x)", d1, d2);

	// 5. blending: half-transparent blue over red
	RenderState bs = rs;
	bs.blend = 1, bs.srcFactor = BLEND_SRC_ALPHA, bs.dstFactor = BLEND_INV_SRC_ALPHA;
	setState(rs);
	quad(300, 200, 400, 250, 0.5f, 0xFF0000FF);
	setState(bs);
	quad(300, 200, 400, 250, 0.5f, 0x0000FF80);
	u32 b = pixel(350, 225);
	int br = (b >> 16) & 0xFF, bb = b & 0xFF;
	ok = br > 110 && br < 145 && bb > 110 && bb < 145 && ((b >> 8) & 0xFF) < 8;
	allOk &= ok;
	NOTE(", blend %s", ok ? "ok" : "WRONG");
	if (!ok) NOTE(" (%08x)", b);

	// 6. program constants
	setState(rs);
	setProgram(fpConst, constFp);
	quad(100, 300, 200, 350, 0.5f, 0xFFFFFFFF);
	u32 c = pixel(150, 325);
	int cr = (c >> 16) & 0xFF, cg = (c >> 8) & 0xFF, cb = c & 0xFF;
	ok = cr > 60 && cr < 68 && cg > 124 && cg < 132 && cb > 187 && cb < 195;
	allOk &= ok;
	NOTE(", constants %s", ok ? "ok" : "WRONG");
	if (!ok) NOTE(" (%08x)", c);

	// 7. scissor: a large quad cut to a small box
	RenderState ss = rs;
	ss.scissor[0] = 420, ss.scissor[1] = 300, ss.scissor[2] = 40, ss.scissor[3] = 20;
	setState(ss);
	setProgram(fpColor, colorFp);
	quad(400, 280, 500, 360, 0.5f, 0x00FF00FF);
	u32 sIn = pixel(430, 310), sAbove = pixel(430, 290), sLeft = pixel(410, 310);
	ok = sIn == 0xFF00FF00 && sAbove == 0xFF102030 && sLeft == 0xFF102030;
	allOk &= ok;
	NOTE(", scissor %s", ok ? "ok" : "WRONG");
	if (!ok) NOTE(" (%08x %08x %08x)", sIn, sAbove, sLeft);
	setState(rs);

	// 8. clearing part of the frame buffer
	const u16 part[4] = { 520, 400, 30, 30 };
	clear(part, 0xFF808080, 0xFFFFFF, true, true, true);
	u32 cIn = pixel(530, 410), cOut = pixel(515, 410);
	ok = cIn == 0xFF808080 && cOut == 0xFF102030;
	allOk &= ok;
	NOTE(", partial clear %s", ok ? "ok" : "WRONG");
	if (!ok) NOTE(" (%08x %08x)", cIn, cOut);

	logf("[rsx] self-test: %s%s\n", report, allOk ? " - all good" : "");
	clear(all, 0xFF000000, 0xFFFFFF, true, true, true);
	waitLabel(0, 1000 * 1000);
	if (t) {
		destroyTexture(t);
	}
	mStateValid = false;
#undef NOTE
}

} // namespace gfx
