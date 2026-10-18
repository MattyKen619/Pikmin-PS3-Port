// PS3 port: a few measurements of what basic operations cost on the PPU, logged once at start-up (see
// ppu_bench.h). They guide the renderer's optimization: the PPU is an in-order processor, and some things
// that are cheap on a PC (converting between integers and floating point, divisions, reading the RSX's
// memory) are not.

#include "ppu_bench.h"

#include "../backend/ps3_backend.h"
#include "../gpu/gpu_cp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace gfx {

namespace {

enum { N = 100000 };

typedef unsigned long long u64;

volatile float sSinkF;
volatile int sSinkI;
volatile u64 sSinkU;

struct Data {
	short s16[N];
	unsigned char bytes[N * 2];
	float a[N], b[N], c[N];
	int ints[N];
	unsigned char out8[N];
};

float sHi[256], sLo[256];

__attribute__((noinline)) u64 kClock()
{
	u64 sum = 0;
	for (int i = 0; i < N; i++) {
		sum += gpu::gfxClock();
	}
	return sum;
}

__attribute__((noinline)) float kIntToFloatCast(const Data& d)
{
	float s = 0;
	for (int i = 0; i < N; i++) {
		s += (float)d.s16[i];
	}
	return s;
}

__attribute__((noinline)) float kIntToFloatTable(const Data& d)
{
	float s = 0;
	for (int i = 0; i < N; i++) {
		s += sHi[d.bytes[i * 2]] + sLo[d.bytes[i * 2 + 1]];
	}
	return s;
}

__attribute__((noinline)) void kFloatToByte(Data& d)
{
	for (int i = 0; i < N; i++) {
		int v      = (int)(d.a[i] * 255.0f + 0.5f);
		d.out8[i] = (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v);
	}
}

__attribute__((noinline)) float kDivChain(const Data& d)
{
	float s = 1.0f;
	for (int i = 0; i < N; i++) {
		s = s / d.a[i];
	}
	return s;
}

__attribute__((noinline)) void kDivIndependent(Data& d)
{
	for (int i = 0; i < N; i++) {
		d.c[i] = d.a[i] / d.b[i];
	}
}

__attribute__((noinline)) void kSqrt(Data& d)
{
	for (int i = 0; i < N; i++) {
		d.c[i] = sqrtf(d.a[i]);
	}
}

__attribute__((noinline)) void kMulAdd(Data& d)
{
	for (int i = 0; i < N; i++) {
		d.c[i] = d.a[i] * d.b[i] + d.c[i];
	}
}

__attribute__((noinline)) float kCompareBranch(const Data& d)
{
	float s = 0;
	for (int i = 0; i < N; i++) {
		float v = d.a[i] - 1.0f;
		s += v < 0 ? 0 : v > 1 ? 1 : v;
	}
	return s;
}

struct Virt {
	virtual int f(int x) = 0;
	virtual ~Virt() { }
};
struct VirtImpl : Virt {
	int f(int x) override { return x + 1; }
};

__attribute__((noinline)) int kVirtual(Virt* v)
{
	int s = 0;
	for (int i = 0; i < N; i++) {
		s = v->f(s);
	}
	return s;
}

double nsPer(u64 ticks, u64 count) { return (double)ticks * 1e9 / (double)ps3b_timebase_freq() / (double)count; }

} // namespace

void ppuBench(void (*log)(const char*), void* rsxMemory, unsigned rsxBytes)
{
	Data* d = (Data*)ps3b_memalign(128, sizeof(Data));
	if (!d) {
		return;
	}
	unsigned seed = 12345;
	for (int i = 0; i < N; i++) {
		seed           = seed * 1103515245u + 12345u;
		d->s16[i]      = (short)(seed >> 8);
		d->bytes[i * 2]     = (unsigned char)(seed >> 16);
		d->bytes[i * 2 + 1] = (unsigned char)(seed >> 24);
		d->a[i]        = 0.5f + (float)((seed >> 9) & 1023) / 512.0f;
		d->b[i]        = 1.0f + (float)((seed >> 13) & 1023) / 1024.0f;
		d->c[i]        = 0.0f;
		d->ints[i]     = (int)(seed >> 4);
	}
	for (int i = 0; i < 256; i++) {
		sHi[i] = (float)(signed char)i * 256.0f;
		sLo[i] = (float)i;
	}
	VirtImpl vi;
	u64 t[16];
	int k = 0;
	t[k++] = ps3b_timebase();
	sSinkU = kClock();
	t[k++] = ps3b_timebase();
	sSinkF = kIntToFloatCast(*d);
	t[k++] = ps3b_timebase();
	sSinkF = kIntToFloatTable(*d);
	t[k++] = ps3b_timebase();
	kFloatToByte(*d);
	t[k++] = ps3b_timebase();
	sSinkF = kDivChain(*d);
	t[k++] = ps3b_timebase();
	kDivIndependent(*d);
	t[k++] = ps3b_timebase();
	kSqrt(*d);
	t[k++] = ps3b_timebase();
	kMulAdd(*d);
	t[k++] = ps3b_timebase();
	sSinkF = kCompareBranch(*d);
	t[k++] = ps3b_timebase();
	sSinkI = kVirtual(&vi);
	t[k++] = ps3b_timebase();

	// memory: copies of 1 MB within main memory and into RSX memory, and reading 64 KB of RSX memory
	const unsigned MB = 1 << 20;
	unsigned char* m1 = (unsigned char*)ps3b_memalign(128, MB);
	unsigned char* m2 = (unsigned char*)ps3b_memalign(128, MB);
	u64 copyMain = 0, copyRsx = 0, readRsx = 0;
	if (m1 && m2) {
		memset(m1, 1, MB);
		memset(m2, 2, MB);
		u64 t0   = ps3b_timebase();
		memcpy(m2, m1, MB);
		copyMain = ps3b_timebase() - t0;
		if (rsxMemory && rsxBytes >= MB) {
			t0      = ps3b_timebase();
			memcpy(rsxMemory, m1, MB);
			copyRsx = ps3b_timebase() - t0;
			t0      = ps3b_timebase();
			memcpy(m2, rsxMemory, 64 * 1024);
			readRsx = ps3b_timebase() - t0;
		}
	}
	char line[600];
	snprintf(line, sizeof(line),
	         "[bench] PPU, ns per operation: time base read %.1f, int to float cast %.1f, by table %.1f, float to byte %.1f, "
	         "divide (chained) %.1f, divide (independent) %.1f, square root %.1f, multiply-add %.1f, clamp with compares %.1f, "
	         "virtual call %.1f; memory copies, MB/s: main %.0f, to RSX memory %.0f, from RSX memory %.0f\n",
	         nsPer(t[1] - t[0], N), nsPer(t[2] - t[1], N), nsPer(t[3] - t[2], N), nsPer(t[4] - t[3], N), nsPer(t[5] - t[4], N),
	         nsPer(t[6] - t[5], N), nsPer(t[7] - t[6], N), nsPer(t[8] - t[7], N), nsPer(t[9] - t[8], N), nsPer(t[10] - t[9], N),
	         copyMain ? 1.0 * ps3b_timebase_freq() / copyMain : 0.0, copyRsx ? 1.0 * ps3b_timebase_freq() / copyRsx : 0.0,
	         readRsx ? 0.0625 * ps3b_timebase_freq() / readRsx : 0.0);
	log(line);
	free(m1);
	free(m2);
	free(d);
}

} // namespace gfx
