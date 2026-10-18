// PS3 port, host tools: draws a frame trace with the PS3 renderer (platform/ps3/rsx/renderer.cpp) on a
// software RSX (soft_device.cpp), and compares the frame with the reference software renderer's.
//
//   gx_rsx TRACE OUT.png [--ref REF.png] [--diff DIFF.png] [--programs FILE] [--efb EFB.png]
//
// Prints how many pixels differ and by how much. --programs writes the text of every fragment program
// the renderer made.

#include "gx_trace_file.h"
#include "soft_device.h"
#include "sw_render.h"

#include "../../../platform/ps3/rsx/renderer.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void logLine(const char* text) { fputs(text, stderr); }

static gfx::Renderer* sRenderer;

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: gx_rsx TRACE OUT.png [--ref REF.png] [--diff DIFF.png] [--efb EFB.png]\n");
		return 2;
	}
	const char* refPath  = NULL;
	const char* diffPath = NULL;
	const char* efbPath  = NULL;
	for (int i = 3; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--ref")) refPath = argv[i + 1];
		if (!strcmp(argv[i], "--diff")) diffPath = argv[i + 1];
		if (!strcmp(argv[i], "--efb")) efbPath = argv[i + 1];
	}
	TraceFile trace;
	if (!trace.load(argv[1])) {
		return 1;
	}

	// the PS3 renderer on the software RSX
	SoftDevice* dev = new SoftDevice;
	{
		gpu::CommandProcessor cp;
		TraceMemory mem(trace);
		gfx::Renderer* r = new gfx::Renderer(cp, mem, *dev);
		sRenderer        = r;
		r->setLogger(logLine);
		cp.setMemory(&mem);
		cp.setListener(r);
		cp.setState(trace.state);
		r->begin();
		replayTrace(trace, cp, mem);
		r->flush();
		const gfx::RendererStats& st = r->stats();
		printf("%s: frame %u (%s)\n", argv[1], trace.frame, trace.label);
		printf("  renderer: %llu draws (%llu skipped, %llu line, %llu point), %llu batches, %llu triangles (%llu culled), "
		       "%llu programs (%llu failed), %llu program binds, %llu texture uploads (%llu KB), %llu missing, %llu copies "
		       "(%llu uses), %llu presents, %llu clears\n",
		       st.draws, st.skippedDraws, st.lineDraws, st.pointDraws, st.batches, st.triangles, st.culled, st.programs,
		       st.programErrors, st.programBinds, st.textureUploads, st.textureUploadBytes >> 10, st.missingTextures, st.copies,
		       st.copyUses, st.presents, st.clears);
		const SoftDevice::Stats& ds = dev->stats();
		printf("  soft RSX: %llu triangles, %llu pixels (%llu killed, %llu written), %llu bad instructions\n", ds.triangles,
		       ds.pixels, ds.killed, ds.written, ds.programErrors);
		delete r;
	}
	const gfx::u32 W = dev->displayWidth(), H = dev->displayHeight();
	std::vector<gpu::u8> rgb(W * H * 3);
	for (gfx::u32 i = 0; i < W * H; i++) {
		gfx::u32 c = dev->display()[i];
		rgb[i * 3] = (gpu::u8)(c >> 16), rgb[i * 3 + 1] = (gpu::u8)(c >> 8), rgb[i * 3 + 2] = (gpu::u8)c;
	}
	swr::writePng(argv[2], rgb.data(), W, H);
	if (efbPath) {
		std::vector<gpu::u8> e(gfx::EFB_WIDTH * gfx::EFB_HEIGHT * 3);
		for (int i = 0; i < gfx::EFB_WIDTH * gfx::EFB_HEIGHT; i++) {
			gfx::u32 c = dev->efb()[i];
			e[i * 3] = (gpu::u8)(c >> 16), e[i * 3 + 1] = (gpu::u8)(c >> 8), e[i * 3 + 2] = (gpu::u8)c;
		}
		swr::writePng(efbPath, e.data(), gfx::EFB_WIDTH, gfx::EFB_HEIGHT);
	}

	// the reference renderer
	{
		gpu::CommandProcessor cp;
		TraceMemory mem(trace);
		static swr::Renderer* ref = new swr::Renderer(cp, mem);
		cp.setMemory(&mem);
		cp.setListener(ref);
		cp.setState(trace.state);
		ref->begin();
		replayTrace(trace, cp, mem);
		if (!ref->framesCopied()) {
			printf("  (no frame from the reference renderer)\n");
			return 0;
		}
		if (refPath) {
			swr::writePng(refPath, ref->frame().data(), ref->frameWidth(), ref->frameHeight());
		}
		const std::vector<gpu::u8>& f = ref->frame();
		const gfx::u32 rw = ref->frameWidth(), rh = ref->frameHeight();
		unsigned long long sum = 0, over8 = 0, over32 = 0, n = 0;
		int maxd = 0;
		std::vector<gpu::u8> diff(W * H * 3, 0);
		for (gfx::u32 y = 0; y < H && y < rh; y++) {
			for (gfx::u32 x = 0; x < W && x < rw; x++) {
				int d = 0;
				for (int k = 0; k < 3; k++) {
					int a = rgb[(y * W + x) * 3 + k], b = f[(y * rw + x) * 3 + k];
					int dd = abs(a - b);
					d      = dd > d ? dd : d;
				}
				sum += d;
				n++;
				over8 += d > 8;
				over32 += d > 32;
				maxd = d > maxd ? d : maxd;
				gpu::u8 v = (gpu::u8)(d * 4 > 255 ? 255 : d * 4);
				diff[(y * W + x) * 3] = v, diff[(y * W + x) * 3 + 1] = v, diff[(y * W + x) * 3 + 2] = v;
			}
		}
		printf("  compared with the reference: mean difference %.3f, %.2f%% of pixels differ by more than 8, %.2f%% by more "
		       "than 32, largest %d\n",
		       n ? (double)sum / n : 0.0, n ? 100.0 * over8 / n : 0.0, n ? 100.0 * over32 / n : 0.0, maxd);
		if (diffPath) {
			swr::writePng(diffPath, diff.data(), W, H);
		}
	}
	return 0;
}
