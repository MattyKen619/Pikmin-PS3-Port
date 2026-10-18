// PS3 port, host tools: draws a frame trace (PIKMIN/gx_trace_*.bin) with the software renderer.
//
//   gx_render TRACE OUT.png [--efb EFB.png] [--copies DIR] [--skip FIRST-LAST]
//
// OUT.png is the frame as copied to the display; --efb also writes the embedded frame buffer at the end,
// --copies writes every texture copied out of the frame buffer.

#include "gx_trace_file.h"
#include "sw_render.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void pageWritten(int group, gpu::u32 address, gpu::u32 size)
{
	if (group == 2) {
		swr::invalidateTextures(address, size);
	}
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: gx_render TRACE OUT.png [--efb EFB.png] [--copies DIR] [--skip FIRST-LAST]\n");
		return 2;
	}
	const char* efbPath  = NULL;
	const char* copyDir  = NULL;
	unsigned long long s0 = 0, s1 = 0;
	for (int i = 3; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--efb")) efbPath = argv[i + 1];
		if (!strcmp(argv[i], "--copies")) copyDir = argv[i + 1];
		if (!strcmp(argv[i], "--skip")) sscanf(argv[i + 1], "%llu-%llu", &s0, &s1);
	}
	TraceFile trace;
	if (!trace.load(argv[1])) {
		return 1;
	}
	gpu::CommandProcessor cp;
	TraceMemory mem(trace);
	mem.onPage = pageWritten;
	static swr::Renderer* r = new swr::Renderer(cp, mem); // (large: the frame buffer)
	if (s0) r->skipDraws(s0, s1);
	cp.setMemory(&mem);
	cp.setListener(r);
	cp.setState(trace.state);
	r->begin();
	replayTrace(trace, cp, mem);

	const swr::Stats& st = r->stats();
	printf("%s: frame %u (%s): %llu draws, %llu triangles (%llu culled, %llu clipped away), %llu pixels shaded, %llu written\n",
	       argv[1], trace.frame, trace.label, (unsigned long long)st.draws, (unsigned long long)st.triangles,
	       (unsigned long long)st.culled, (unsigned long long)st.clipped, (unsigned long long)st.pixels,
	       (unsigned long long)st.pixelsWritten);
	printf("  not drawn: %llu line draws, %llu point draws; unsupported: fog in %llu draws, indirect textures in %llu, dual "
	       "texture matrices in %llu, %llu emboss texgens; %llu missing textures, %llu uses of copied textures, %llu TEV compare stages\n",
	       (unsigned long long)st.lines, (unsigned long long)st.points, (unsigned long long)st.fogDraws,
	       (unsigned long long)st.indirectDraws, (unsigned long long)st.dualTexDraws, (unsigned long long)st.embossTexgens,
	       (unsigned long long)st.missingTextures, (unsigned long long)st.copyTextureUses, (unsigned long long)st.tevCompareStages);
	if (r->framesCopied()) {
		swr::writePng(argv[2], r->frame().data(), r->frameWidth(), r->frameHeight());
		printf("  frame %ux%u -> %s\n", r->frameWidth(), r->frameHeight(), argv[2]);
	} else {
		printf("  no copy to the display\n");
	}
	if (efbPath) {
		std::vector<gpu::u8> rgb(swr::EFB_WIDTH * swr::EFB_HEIGHT * 3);
		for (int i = 0; i < swr::EFB_WIDTH * swr::EFB_HEIGHT; i++) {
			gpu::u32 c = r->efbColor()[i];
			rgb[i * 3] = c >> 24, rgb[i * 3 + 1] = c >> 16, rgb[i * 3 + 2] = c >> 8;
		}
		swr::writePng(efbPath, rgb.data(), swr::EFB_WIDTH, swr::EFB_HEIGHT);
	}
	if (copyDir) {
		for (std::map<gpu::u32, swr::CopyTexture>::const_iterator it = r->copyTextures().begin(); it != r->copyTextures().end(); ++it) {
			const swr::CopyTexture& t = it->second;
			std::vector<gpu::u8> rgb(t.width * t.height * 3);
			for (gpu::u32 i = 0; i < t.width * t.height; i++) {
				rgb[i * 3] = t.rgba[i] >> 24, rgb[i * 3 + 1] = t.rgba[i] >> 16, rgb[i * 3 + 2] = t.rgba[i] >> 8;
			}
			char path[512];
			snprintf(path, sizeof(path), "%s/copy_%07x_fmt%u_%ux%u.png", copyDir, it->first, t.format, t.width, t.height);
			swr::writePng(path, rgb.data(), t.width, t.height);
		}
	}
	return 0;
}
