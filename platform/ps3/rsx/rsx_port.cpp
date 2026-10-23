// PS3 port: starting the RSX renderer, frame captures and status (see rsx_port.h).

#include "rsx_port.h"

#include "device_rsx.h"
#include "ppu_bench.h"
#include "renderer.h"

#include "../backend/ps3_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static gfx::RsxDevice* sDev;
static gfx::Renderer* sRenderer;
static bool sTestButtons; // (PIKMIN/test_buttons.txt: ps3RsxDebugButtons)

static void logLine(const char* text) { ps3b_log_write(text); }

static void outOfMemory(size_t bytes)
{
	char buf[160];
	snprintf(buf, sizeof(buf), "[rsx] FAILED: out of memory (%u bytes asked for); the game stops\n", (unsigned)bytes);
	ps3b_log_write(buf);
	ps3b_log_memory("out of memory");
}

gpu::Listener* ps3rsx::start(gpu::CommandProcessor& cp, gpu::Memory& memory)
{
	gfx::allocFailedHook() = outOfMemory;
	if (ps3b_data_root()) {
		char path[256];
		snprintf(path, sizeof(path), "%s/no_rsx.txt", ps3b_data_root());
		if (ps3b_file_exists(path)) {
			ps3b_log_write("[rsx] PIKMIN/no_rsx.txt exists: not drawing (the text console stays on screen)\n");
			return NULL;
		}
	}
	sDev = gfx::create<gfx::RsxDevice>();
	if (!sDev->init()) {
		return NULL;
	}
	sDev->selfTest();
	{
		gfx::Texture* t = sDev->createTexture(512, 512); // (1 MB of RSX memory to time copies with)
		gfx::ppuBench(logLine, t ? t->texels : NULL, t ? t->bytes : 0);
		if (t) {
			sDev->destroyTexture(t);
		}
	}
	sRenderer = gfx::create<gfx::Renderer>(cp, memory, *sDev);
	sRenderer->setLogger(logLine);
	if (ps3b_data_root()) {
		char path[256];
		snprintf(path, sizeof(path), "%s/cpu_xf.txt", ps3b_data_root());
		if (ps3b_file_exists(path)) {
			sRenderer->setGpuTransform(false);
			ps3b_log_write("[rsx] PIKMIN/cpu_xf.txt exists: vertices are transformed on the CPU\n");
		}
	}
	ps3b_log_write(sRenderer->gpuTransform() ? "[rsx] vertices are transformed and lit by the RSX (vertex programs)\n"
	                                         : "[rsx] vertices are transformed and lit on the CPU\n");
	if (ps3b_data_root()) {
		char path[256];
		snprintf(path, sizeof(path), "%s/no_vertex_cache.txt", ps3b_data_root());
		if (ps3b_file_exists(path)) {
			sRenderer->setVertexCache(false);
			ps3b_log_write("[rsx] PIKMIN/no_vertex_cache.txt exists: the vertex cache is off\n");
		}
	}
	ps3b_log_write(sRenderer->vertexCache() ? "[rsx] display list draws are kept in the vertex cache\n"
	                                        : "[rsx] the vertex cache is off\n");
	if (ps3b_data_root()) {
		char path[256];
		snprintf(path, sizeof(path), "%s/test_buttons.txt", ps3b_data_root());
		sTestButtons = ps3b_file_exists(path) != 0;
		if (sTestButtons) {
			ps3b_log_write("[rsx] PIKMIN/test_buttons.txt exists: R3, L3 and Select are test buttons\n");
		}
		// Audit frames (the vertex cache and the change checks compared with a fresh look, once a minute)
		// found nothing wrong since test build 30, and each one held the graphics thread up for about 100 ms:
		// a hitch, and (test build 39) sound waiting to be mixed. Only with PIKMIN/audit.txt now.
		snprintf(path, sizeof(path), "%s/audit.txt", ps3b_data_root());
		const bool audit = ps3b_file_exists(path) != 0;
		sRenderer->setAuditFrames(audit ? 1800 : 0);
		if (audit) {
			ps3b_log_write("[rsx] PIKMIN/audit.txt exists: one frame a minute is an audit frame\n");
		}
	}
	sRenderer->begin();
	return sRenderer;
}

// The picture is written to the USB drive by a thread of its own: up to test build 39 the graphics thread
// wrote it, 300-600 ms during which it drew nothing (a hitch) and mixed no sound (a stutter).
namespace {

struct CaptureFile {
	char path[256];
	char name[64];
	unsigned char* data;
	unsigned int size, w, h;
	unsigned long long readMs;
};

void captureWriter(void* arg)
{
	CaptureFile* c         = (CaptureFile*)arg;
	unsigned long long t0  = ps3b_timebase();
	int fd                 = ps3b_file_open_rw(c->path, 1);
	int wrote              = -1;
	if (fd >= 0) {
		wrote = ps3b_file_pwrite(fd, c->data, 0, c->size);
		ps3b_file_close(fd);
	}
	unsigned long long ms = (ps3b_timebase() - t0) * 1000ULL / ps3b_timebase_freq();
	char line[220];
	snprintf(line, sizeof(line), "[rsx] frame drawn by the RSX written to PIKMIN/%s.ppm (%ux%u, %s; read back in %llu ms, written in %llu ms)\n",
	         c->name, c->w, c->h, wrote == (int)c->size ? "ok" : "write FAILED", c->readMs, ms);
	ps3b_log_write(line);
	free(c->data);
	free(c);
	ps3b_thread_exit();
}

} // namespace

void ps3rsx::captureFrame(const gpu::CopyInfo& info, const char* name)
{
	if (!sRenderer || !ps3b_data_root()) {
		return;
	}
	gfx::u32 w = info.width, h = info.height;
	if (info.srcLeft + w > gfx::EFB_WIDTH) w = gfx::EFB_WIDTH - info.srcLeft;
	if (info.srcTop + h > gfx::EFB_HEIGHT) h = gfx::EFB_HEIGHT - info.srcTop;
	gfx::Vec<gfx::u32> px(w * h);
	unsigned long long t0 = ps3b_timebase();
	if (!sRenderer->readEfb(&px[0], info.srcLeft, info.srcTop, w, h)) {
		ps3b_log_write("[rsx] could not read the frame back from the RSX\n");
		return;
	}
	CaptureFile* c = (CaptureFile*)malloc(sizeof(CaptureFile));
	char header[64];
	int hl         = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", w, h);
	unsigned char* file = (unsigned char*)malloc(hl + w * h * 3);
	if (!c || !file) {
		free(c);
		free(file);
		ps3b_log_write("[rsx] no memory to write the frame capture\n");
		return;
	}
	memcpy(file, header, hl);
	for (gfx::u32 i = 0; i < w * h; i++) {
		gfx::u32 v          = px[i];
		file[hl + i * 3]     = (unsigned char)(v >> 16);
		file[hl + i * 3 + 1] = (unsigned char)(v >> 8);
		file[hl + i * 3 + 2] = (unsigned char)v;
	}
	snprintf(c->path, sizeof(c->path), "%s/%s.ppm", ps3b_data_root(), name);
	snprintf(c->name, sizeof(c->name), "%s", name);
	c->data   = file;
	c->size   = (unsigned int)(hl + w * h * 3);
	c->w      = w;
	c->h      = h;
	c->readMs = (ps3b_timebase() - t0) * 1000ULL / ps3b_timebase_freq();
	if (ps3b_thread_create(captureWriter, c, 1450, 0x8000, "pikmin_capture_ppm") != 0) {
		ps3b_log_write("[rsx] could not start the thread that writes the frame capture\n");
		free(file);
		free(c);
	}
}

extern "C" void ps3GxRecordRequested(void); // (gx_trace.cpp)

// Test buttons, if PIKMIN/test_buttons.txt exists (the game does not use these buttons, but a stick is easily
// pressed in by accident). R3: the next test mode (Renderer::MODE_*). L3: textures and vertex arrays checked
// for changes every frame, or as usual. Each press is logged, and the squares beside the picture show what is
// on (renderer.cpp, present). Select: records the next frame.
extern "C" void ps3RsxDebugButtons(unsigned int buttons)
{
	static unsigned int sLast;
	const unsigned int pressed = buttons & ~sLast;
	sLast = buttons;
	if (!sRenderer || !pressed || !sTestButtons) {
		return;
	}
	if (pressed & PS3B_BTN_R3) {
		sRenderer->requestSwitch(gfx::Renderer::SWITCH_MODE);
	}
	if (pressed & PS3B_BTN_L3) {
		sRenderer->requestSwitch(gfx::Renderer::SWITCH_CHECKS);
	}
	if ((pressed & PS3B_BTN_SELECT) && !(buttons & PS3B_BTN_START)) {
		ps3GxRecordRequested();
	}
}

void ps3rsx::sectionChanged()
{
	if (sRenderer) {
		sRenderer->requestCacheReset();
	}
}

void ps3rsx::status(char* buf, unsigned int size)
{
	if (!sRenderer) {
		snprintf(buf, size, "RSX not drawing");
		return;
	}
	static gfx::RendererStats last;
	static gfx::u32 lastFrames;
	static gfx::u32 sLastVpLoads, sLastVpStarts;
	const gfx::RendererStats& s = sRenderer->stats();
	gfx::u32 frames             = sRenderer->framesPresented();
	gfx::u32 n                  = frames - lastFrames ? frames - lastFrames : 1;
	int at = snprintf(buf, size,
	         "RSX: %u frames shown; per frame %llu draws in %llu batches, %llu program changes, %llu culled of %llu triangles, "
	         "%llu KB of textures uploaded; %llu programs (%llu failed), %u textures (%u KB), %llu skipped, %llu line/point "
	         "draws, %llu missing textures, %llu copies, %llu waits; RSX memory %u KB; %llu of %u frames finished before the RSX "
	         "had drawn the previous one",
	         frames, (s.draws - last.draws) / n, (s.batches - last.batches) / n, (s.programBinds - last.programBinds) / n,
	         (s.culled - last.culled) / n, (s.triangles - last.triangles) / n, ((s.textureUploadBytes - last.textureUploadBytes) >> 10) / n,
	         s.programs, s.programErrors, s.textures, s.textureBytes >> 10, s.skippedDraws, s.lineDraws + s.pointDraws,
	         s.missingTextures, s.copies, s.fenceWaits, sDev->localMemoryUsed() >> 10, s.rsxBehind - last.rsxBehind, frames - lastFrames);
	if (at > 0 && at < (int)size) {
		at += snprintf(buf + at, size - at,
		               "; transform: per frame %llu draws on the RSX, %llu on the CPU, %llu matrix rows and %llu set-ups loaded; %llu vertex "
		               "programs (%llu failed), per frame %u loaded into the RSX and %u switched to there",
		               (s.gpuDraws - last.gpuDraws) / n, (s.cpuDraws - last.cpuDraws) / n, (s.matrixRows - last.matrixRows) / n,
		               (s.setupUploads - last.setupUploads) / n, s.xfPrograms, s.xfProgramErrors, (sDev->vertexProgramLoads() - sLastVpLoads) / n,
		               (sDev->vertexProgramStarts() - sLastVpStarts) / n);
		sLastVpLoads  = sDev->vertexProgramLoads();
		sLastVpStarts = sDev->vertexProgramStarts();
	}
	if (at > 0 && at < (int)size) {
		if (sRenderer->vertexCache()) {
			snprintf(buf + at, size - at,
			         "; vertex cache: per frame %llu draws from it (%llu found by following the last), %llu unpacked into it, %llu not "
			         "cached; %u entries, %u KB; %llu array checks (%llu changed), started over %llu times; since the last report: "
			         "%llu variants made, %llu refills (CP registers %llu, list bytes %llu, array places %llu, array contents %llu)",
			         (s.cacheHits - last.cacheHits) / n, (s.cacheFollowed - last.cacheFollowed) / n, (s.cacheFills - last.cacheFills) / n,
			         (s.cacheUncached - last.cacheUncached) / n,
			         s.cacheEntries, s.cacheBytes >> 10, (s.regionChecks - last.regionChecks) / n, s.regionChanges - last.regionChanges,
			         s.cacheResets, s.cacheVariants - last.cacheVariants,
			         (s.refillWhy[1] + s.refillWhy[2] + s.refillWhy[3] + s.refillWhy[4]) - (last.refillWhy[1] + last.refillWhy[2] + last.refillWhy[3] + last.refillWhy[4]),
			         s.refillWhy[1] - last.refillWhy[1], s.refillWhy[2] - last.refillWhy[2], s.refillWhy[3] - last.refillWhy[3],
			         s.refillWhy[4] - last.refillWhy[4]);
		} else {
			snprintf(buf + at, size - at, "; vertex cache off");
		}
	}
	last       = s;
	lastFrames = frames;
}
