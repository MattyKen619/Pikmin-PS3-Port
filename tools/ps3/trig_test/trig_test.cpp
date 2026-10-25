// tools/ps3/trig_test: checks the PS3 port's sinf and cosf (platform/ps3/sdk/ps3_trig.h) against the
// correctly rounded values, on the host: error in units in the last place, over every float in a range of
// angles the game uses (stepped), and random angles up to the largest the fast path takes.
//
//     g++ -O2 -o trig_test trig_test.cpp && ./trig_test

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../../platform/ps3/sdk/ps3_trig.h"

static double fb(double x) { return sin(x); }
static double fbc(double x) { return cos(x); }

static int64_t ulpDiff(float a, float b)
{
	int32_t ia, ib;
	memcpy(&ia, &a, 4);
	memcpy(&ib, &b, 4);
	if (ia < 0) ia = (int32_t)0x80000000 - ia;
	if (ib < 0) ib = (int32_t)0x80000000 - ib;
	const int64_t d = (int64_t)ia - ib;
	return d < 0 ? -d : d;
}

struct Stats {
	uint64_t n = 0, off = 0;
	int64_t worst = 0;
	float worstAt = 0;
	void add(float x, float got, float want)
	{
		n++;
		const int64_t d = ulpDiff(got, want);
		off += d != 0;
		if (d > worst) {
			worst   = d;
			worstAt = x;
		}
	}
};

int main()
{
	Stats s, c;
	auto check = [&](float x) {
		s.add(x, ps3trig::sinf(x, fb), (float)sinl((long double)x));
		c.add(x, ps3trig::cosf(x, fbc), (float)cosl((long double)x));
	};
	// every float in [-64, 64], one in 7 (and all the ones near 0 that are tiny)
	uint32_t bits;
	for (bits = 0; bits < 0x42800000u; bits += 7) {
		float x;
		memcpy(&x, &bits, 4);
		check(x);
		check(-x);
	}
	// random angles up to 1e5
	uint64_t r = 88172645463325252ULL;
	for (int i = 0; i < 20000000; i++) {
		r ^= r << 13, r ^= r >> 7, r ^= r << 17;
		const float x = (float)((double)(r >> 11) / 9007199254740992.0 * 2e5 - 1e5);
		check(x);
	}
	// special values
	const float sp[] = { 0.0f, -0.0f, 1e-30f, -1e-30f, 3.14159265f, 1.57079633f, 1e5f, -1e5f, 1e6f, 1e30f };
	for (float x : sp) {
		check(x);
	}
	printf("sinf: %llu angles, %llu not correctly rounded, at most %lld ulp (at %.9g)\n", (unsigned long long)s.n, (unsigned long long)s.off,
	       (long long)s.worst, s.worstAt);
	printf("cosf: %llu angles, %llu not correctly rounded, at most %lld ulp (at %.9g)\n", (unsigned long long)c.n, (unsigned long long)c.off,
	       (long long)c.worst, c.worstAt);
	printf("sin(-0) %s, sin(inf) %s, cos(nan) %s\n", signbit(ps3trig::sinf(-0.0f, fb)) ? "-0" : "+0",
	       isnan(ps3trig::sinf(INFINITY, fb)) ? "nan" : "not nan", isnan(ps3trig::cosf(NAN, fbc)) ? "nan" : "not nan");
	return s.worst > 1 || c.worst > 1;
}
