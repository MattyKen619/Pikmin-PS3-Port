// PS3 port: reading frame traces (format: platform/ps3/gpu/gx_trace.cpp) on the host.

#ifndef GX_TRACE_FILE_H
#define GX_TRACE_FILE_H

#include "../../../platform/ps3/gpu/gpu_cp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

struct TraceEvent {
	enum Type { FIFO, MEM } type;
	gpu::u32 address, size, kind;
	const gpu::u8* data; // FIFO bytes or memory contents
};

struct TraceFile {
	std::vector<gpu::u8> bytes;
	std::vector<TraceEvent> events;
	gpu::u32 frame;
	char label[33];
	gpu::State state;
	gpu::u32 version;
	gpu::u32 fifoBytes, memEvents, memBytes;
	gpu::u32 fifoLimit; // FIFO bytes that make up the frame (version 2); 0xFFFFFFFF: up to the first copy

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
		version   = be32(p + 8);
		frame     = be32(p + 12);
		fifoLimit = 0xFFFFFFFF;
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
		while (end - p >= 4) {
			gpu::u32 tag = be32(p);
			p += 4;
			if (tag == 0x454E4420) { // 'END '
				if (version >= 2 && end - p >= 4) {
					fifoLimit = be32(p);
				}
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
		return true;
	}
};

// Serves the processor's memory reads from the trace: normally the next recorded block in order.
struct TraceMemory : gpu::Memory {
	const TraceFile& trace;
	size_t cursor;
	explicit TraceMemory(const TraceFile& t)
	    : trace(t)
	    , cursor(0)
	{
	}
	const gpu::u8* read(gpu::u32 address, gpu::u32 size, gpu::MemKind kind) override
	{
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

// Runs the trace through a command processor.
inline void replayTrace(const TraceFile& trace, gpu::CommandProcessor& cp, TraceMemory& mem)
{
	cp.state()   = trace.state;
	gpu::u32 fed = 0;
	for (size_t i = 0; i < trace.events.size(); i++) {
		const TraceEvent& e = trace.events[i];
		if (e.type != TraceEvent::FIFO) {
			continue;
		}
		if (mem.cursor < i) {
			mem.cursor = i; // the reads this chunk triggers were recorded after it
		}
		gpu::u32 n = e.size;
		if (n > trace.fifoLimit - fed) {
			n = trace.fifoLimit - fed;
		}
		// (in pieces, as the console interprets them)
		for (gpu::u32 k = 0; k < n; k += 16384) {
			cp.write(e.data + k, n - k < 16384 ? n - k : 16384);
		}
		fed += n;
		if (fed >= trace.fifoLimit) {
			break;
		}
	}
}

#endif
