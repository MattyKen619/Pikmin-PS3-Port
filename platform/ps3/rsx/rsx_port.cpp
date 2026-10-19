// PS3 port: starting the RSX renderer, frame captures and status (see rsx_port.h).

#include "rsx_port.h"

#include "device_rsx.h"
#include "ppu_bench.h"
#include "renderer.h"

#include "../backend/ps3_backend.h"

#include <stdio.h>
#include <string.h>

static gfx::RsxDevice* sDev;
static gfx::Renderer* sRenderer;

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
	sRenderer->begin();
	return sRenderer;
}

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
	char header[64];
	int hl = snprintf(header, sizeof(header), "P6\n%u %u\n255\n", w, h);
	gfx::Vec<unsigned char> file(hl + w * h * 3);
	memcpy(&file[0], header, hl);
	for (gfx::u32 i = 0; i < w * h; i++) {
		gfx::u32 c          = px[i];
		file[hl + i * 3]     = (unsigned char)(c >> 16);
		file[hl + i * 3 + 1] = (unsigned char)(c >> 8);
		file[hl + i * 3 + 2] = (unsigned char)c;
	}
	char path[256];
	snprintf(path, sizeof(path), "%s/%s.ppm", ps3b_data_root(), name);
	int fd = ps3b_file_open_rw(path, 1);
	if (fd < 0) {
		ps3b_log_write("[rsx] could not create the frame capture file\n");
		return;
	}
	int wrote = ps3b_file_pwrite(fd, &file[0], 0, (unsigned int)file.size());
	ps3b_file_close(fd);
	unsigned long long ms = (ps3b_timebase() - t0) * 1000ULL / ps3b_timebase_freq();
	char line[200];
	snprintf(line, sizeof(line), "[rsx] frame drawn by the RSX written to PIKMIN/%s.ppm (%ux%u, %s, %llu ms)\n", name, w, h,
	         wrote == (int)file.size() ? "ok" : "write FAILED", ms);
	ps3b_log_write(line);
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
		               "programs (%llu failed)",
		               (s.gpuDraws - last.gpuDraws) / n, (s.cpuDraws - last.cpuDraws) / n, (s.matrixRows - last.matrixRows) / n,
		               (s.setupUploads - last.setupUploads) / n, s.xfPrograms, s.xfProgramErrors);
	}
	if (at > 0 && at < (int)size) {
		if (sRenderer->vertexCache()) {
			snprintf(buf + at, size - at,
			         "; vertex cache: per frame %llu draws from it, %llu unpacked into it, %llu not cached; %u entries, %u KB; "
			         "%llu array checks (%llu changed), started over %llu times",
			         (s.cacheHits - last.cacheHits) / n, (s.cacheFills - last.cacheFills) / n, (s.cacheUncached - last.cacheUncached) / n,
			         s.cacheEntries, s.cacheBytes >> 10, (s.regionChecks - last.regionChecks) / n, s.regionChanges - last.regionChanges,
			         s.cacheResets);
		} else {
			snprintf(buf + at, size - at, "; vertex cache off");
		}
	}
	last       = s;
	lastFrames = frames;
}
