// PS3 port: reading frame traces (format: platform/ps3/gpu/gx_trace.cpp) on the host.

#ifndef GX_TRACE_FILE_H
#define GX_TRACE_FILE_H

#include "../../../platform/ps3/gpu/gpu_cp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <vector>

struct TraceEvent {
	enum Type { FIFO, MEM, PAGE } type;
	gpu::u32 address, size, kind;
	const gpu::u8* data; // FIFO bytes or memory contents
};

struct TraceFile {
	std::vector<gpu::u8> bytes;
	std::vector<TraceEvent> events;
	gpu::u32 version;
	gpu::u32 frame;
	char label[33];
	gpu::State state;
	gpu::u32 fifoBytes, memEvents, memBytes;
	gpu::u32 fifoLimit;  // FIFO bytes that make up the frame (version 2+); 0xFFFFFFFF: up to the first copy
	bool incomplete;     // the console's buffer filled up before the frame ended (version 3)

	static gpu::u32 be32(const gpu::u8* p) { return ((gpu::u32)p[0] << 24) | ((gpu::u32)p[1] << 16) | ((gpu::u32)p[2] << 8) | p[3]; }

	bool load(const char* path)
	{
		FILE* f = fopen(path, "rb");
		if (!f) {
			fprintf(stderr, "cannot open %s\n", path);
			return false;
		}
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		bytes.resize(n);
		if (fread(bytes.data(), 1, n, f) != (size_t)n) {
			fclose(f);
			return false;
		}
		fclose(f);
		const gpu::u8* p   = bytes.data();
		const gpu::u8* end = p + n;
		if (n < 56 || memcmp(p, "GXTRACE1", 8) != 0) {
			fprintf(stderr, "%s: not a frame trace\n", path);
			return false;
		}
		version    = be32(p + 8);
		frame      = be32(p + 12);
		fifoLimit  = 0xFFFFFFFF;
		incomplete = false;
		memcpy(label, p + 16, 32);
		label[32] = 0;
		p += 48;
		const gpu::u32 words = 0x100 + 0x100 + 0x1100;
		if (end - p < (long)(words * 4)) {
			return false;
		}
		for (int i = 0; i < 0x100; i++, p += 4) {
			state.cp[i] = be32(p);
		}
		for (int i = 0; i < 0x100; i++, p += 4) {
			state.bp[i] = be32(p);
		}
		for (int i = 0; i < 0x1100; i++, p += 4) {
			state.xf[i] = be32(p);
		}
		fifoBytes = memEvents = memBytes = 0;
		bool ended = false;
		while (end - p >= 4) {
			gpu::u32 tag = be32(p);
			p += 4;
			if (tag == 0x454E4420) { // 'END '
				if (version >= 2 && end - p >= 4) {
					fifoLimit = be32(p);
				}
				if (version >= 3 && end - p >= 8) {
					incomplete = be32(p + 4) & 1;
				}
				ended = true;
				break;
			}
			TraceEvent e;
			if (tag == 0x4649464F) { // 'FIFO'
				gpu::u32 len = be32(p);
				e.type       = TraceEvent::FIFO;
				e.size       = len;
				e.data       = p + 4;
				e.address = e.kind = 0;
				p += 4 + ((len + 3) & ~3u);
				fifoBytes += len;
			} else if (tag == 0x50414745) { // 'PAGE'
				e.type    = TraceEvent::PAGE;
				e.kind    = be32(p);
				e.address = be32(p + 4);
				e.size    = be32(p + 8);
				e.data    = p + 12;
				p += 12 + ((e.size + 3) & ~3u);
				memEvents++;
				memBytes += e.size;
			} else if (tag == 0x4D454D20 || tag == 0x4D524546) { // 'MEM ' / 'MREF'
				e.type    = TraceEvent::MEM;
				e.address = be32(p);
				e.size    = be32(p + 4);
				e.kind    = be32(p + 8);
				if (tag == 0x4D454D20) {
					e.data = p + 12;
					p += 12 + ((e.size + 3) & ~3u);
					memBytes += e.size;
				} else {
					e.data = bytes.data() + be32(p + 12);
					p += 16;
				}
				memEvents++;
			} else {
				fprintf(stderr, "%s: unknown event 0x%08x at offset %ld (truncated trace?)\n", path, tag, (long)(p - 4 - bytes.data()));
				break;
			}
			if (p > end) {
				fprintf(stderr, "%s: event past the end of the file\n", path);
				break;
			}
			events.push_back(e);
		}
		if (!ended) {
			incomplete = true;
		}
		return true;
	}
};

// Serves the processor's memory reads during a replay.
//   Version 3: from memory images (one per address space: display lists 32-bit, vertex arrays 30-bit,
//   textures and TLUTs 26-bit) that the trace's pages are copied into as the replay reaches them.
//   Versions 1-2: from the recorded blocks, normally the next one in order.
struct TraceMemory : gpu::Memory {
	const TraceFile& trace;
	size_t cursor;
	gpu::u8* image[3];
	std::vector<gpu::u8> present[3]; // one bit per 4 KB page
	void (*onPage)(int group, gpu::u32 address, gpu::u32 size); // a page was copied into an image
	static gpu::u32 spaceBits(int g) { return g == 0 ? 32 : g == 1 ? 30 : 26; }
	static int group(gpu::u32 kind) { return kind == gpu::MEM_DISPLAY_LIST ? 0 : kind == gpu::MEM_ARRAY ? 1 : 2; }

	explicit TraceMemory(const TraceFile& t)
	    : trace(t)
	    , cursor(0)
	    , onPage(NULL)
	{
		for (int g = 0; g < 3; g++) {
			image[g] = NULL;
		}
		if (t.version >= 3) {
			for (int g = 0; g < 3; g++) {
				size_t size = (size_t)1 << spaceBits(g);
				void* m     = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
				image[g]    = m == MAP_FAILED ? NULL : (gpu::u8*)m;
				present[g].assign((size >> 12) / 8 + 1, 0);
			}
		}
	}
	~TraceMemory()
	{
		for (int g = 0; g < 3; g++) {
			if (image[g]) {
				munmap(image[g], (size_t)1 << spaceBits(g));
			}
		}
	}

	void addPage(const TraceEvent& e)
	{
		int g = group(e.kind);
		if (!image[g]) {
			return;
		}
		gpu::u64 mask = ((gpu::u64)1 << spaceBits(g)) - 1;
		gpu::u64 a    = e.address & mask;
		if (a + e.size > mask + 1) {
			return;
		}
		memcpy(image[g] + a, e.data, e.size);
		if (onPage) {
			onPage(g, (gpu::u32)a, e.size);
		}
		for (gpu::u64 page = a >> 12; page <= (a + e.size - 1) >> 12 && e.size; page++) {
			present[g][page >> 3] |= (gpu::u8)(1 << (page & 7));
		}
	}

	const gpu::u8* read(gpu::u32 address, gpu::u32 size, gpu::MemKind kind) override
	{
		if (trace.version >= 3) {
			int g = group(kind);
			if (!image[g] || !size) {
				return NULL;
			}
			gpu::u64 mask = ((gpu::u64)1 << spaceBits(g)) - 1;
			gpu::u64 a    = address & mask;
			if (a + size > mask + 1) {
				return NULL;
			}
			for (gpu::u64 page = a >> 12; page <= (a + size - 1) >> 12; page++) {
				if (!(present[g][page >> 3] & (1 << (page & 7)))) {
					return NULL;
				}
			}
			return image[g] + a;
		}
		const std::vector<TraceEvent>& ev = trace.events;
		for (size_t pass = 0; pass < 2; pass++) {
			size_t from = pass == 0 ? cursor : 0;
			size_t to   = pass == 0 ? ev.size() : cursor;
			for (size_t i = from; i < to; i++) {
				const TraceEvent& e = ev[i];
				if (e.type == TraceEvent::MEM && e.address == address && e.size >= size && e.kind == (gpu::u32)kind) {
					if (pass == 0) {
						cursor = i + 1;
					}
					return e.data;
				}
			}
		}
		return NULL;
	}
};

// Runs the trace through a command processor: the frame's FIFO bytes (in pieces, as the console
// interprets them), with the memory pages in place before the commands that read them.
inline void replayTrace(const TraceFile& trace, gpu::CommandProcessor& cp, TraceMemory& mem)
{
	cp.state()   = trace.state;
	gpu::u32 fed = 0;
	for (size_t i = 0; i < trace.events.size() && fed < trace.fifoLimit; i++) {
		const TraceEvent& e = trace.events[i];
		if (e.type == TraceEvent::PAGE) {
			mem.addPage(e);
			continue;
		}
		if (e.type != TraceEvent::FIFO) {
			continue;
		}
		if (mem.cursor < i) {
			mem.cursor = i; // (versions 1-2: the reads this chunk triggers were recorded after it)
		}
		gpu::u32 n = e.size;
		if (n > trace.fifoLimit - fed) {
			n = trace.fifoLimit - fed;
		}
		for (gpu::u32 k = 0; k < n; k += 16384) {
			cp.write(e.data + k, n - k < 16384 ? n - k : 16384);
		}
		fed += n;
	}
}

#endif
