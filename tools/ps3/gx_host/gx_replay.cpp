// PS3 port: replays a frame trace (PIKMIN/gx_trace_*.bin, see platform/ps3/gpu/gx_trace.cpp) through the
// port's command processor on the host, and reports what the frame contains.
//
//   gx_replay TRACE [-v]
//
// The processor is the one the PS3 build uses (platform/ps3/gpu/gpu_cp.cpp); memory reads are served
// from the blocks recorded in the trace.

#include "../../../platform/ps3/gpu/gpu_cp.h"
#include "gx_trace_file.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace gpu;

struct Summary : Listener {
	bool verbose;
	u32 draws, vertices, copies, xfb;
	u32 prims[8];
	void draw(const DrawCall& c) override
	{
		if (xfb) {
			return; // past the end of the recorded frame
		}
		draws++;
		vertices += c.count;
		prims[(c.primitive >> 3) & 7]++;
		if (verbose) {
			const Vertex& v = c.vertices[0];
			printf("  draw %4u: prim %02x vat %u, %4u vertices%s; first at (%.3f %.3f %.3f) mtx %u\n", draws, c.primitive, c.vat,
			       c.count, c.inDisplayList ? " (display list)" : "", v.pos[0], v.pos[1], v.pos[2], v.pnmtx);
		}
	}
	void copy(const CopyInfo& info) override
	{
		if (xfb) {
			return;
		}
		copies++;
		xfb += info.toXfb;
		if (verbose) {
			printf("  copy %ux%u at (%u,%u) to %s 0x%08x%s\n", info.width, info.height, info.srcLeft, info.srcTop,
			       info.toXfb ? "display" : "texture", info.dest, info.clear ? ", clear" : "");
		}
	}
	void badCommand(u8 op, const u8* where, u32 avail, bool inDl) override
	{
		(void)where;
		(void)avail;
		printf("  BAD COMMAND 0x%02x%s\n", op, inDl ? " in a display list" : "");
	}
	void memoryFailed(u32 address, u32 size, MemKind kind) override
	{
		printf("  MEMORY NOT IN TRACE: 0x%08x (%u bytes, kind %d)\n", address, size, (int)kind);
	}
};

int main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: gx_replay TRACE [-v]\n");
		return 2;
	}
	TraceFile trace;
	if (!trace.load(argv[1])) {
		return 1;
	}
	Summary sum;
	sum.draws = sum.vertices = sum.copies = sum.xfb = 0;
	memset(sum.prims, 0, sizeof(sum.prims));
	sum.verbose = argc > 2 && strcmp(argv[2], "-v") == 0;

	CommandProcessor cp;
	TraceMemory mem(trace);
	cp.setMemory(&mem);
	cp.setListener(&sum);
	replayTrace(trace, cp, mem);

	printf("%s: frame %u (%s): %u draws, %u vertices, %u copies (%u to the display); %u FIFO bytes, %u memory blocks (%u KB)\n",
	       argv[1], trace.frame, trace.label, sum.draws, sum.vertices, sum.copies, sum.xfb, trace.fifoBytes, trace.memEvents,
	       trace.memBytes >> 10);
	static const char* const kPrim[8] = { "quads", "quads2", "triangles", "strips", "fans", "lines", "linestrips", "points" };
	printf("  primitives:");
	for (int i = 0; i < 8; i++) {
		if (sum.prims[i]) {
			printf(" %s %u", kPrim[i], sum.prims[i]);
		}
	}
	printf("\n  processor: %llu bad commands, %llu memory failures, %llu nested display lists\n",
	       (unsigned long long)cp.stats().badCommands, (unsigned long long)cp.stats().memoryFailures,
	       (unsigned long long)cp.stats().nestedDisplayLists);
	return cp.stats().badCommands || cp.stats().memoryFailures ? 1 : 0;
}
