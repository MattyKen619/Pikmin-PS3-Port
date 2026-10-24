// tools/ps3/dsp_test: the backend calls an earlier dsp_hle.cpp makes, on the host (OLD=...).
#pragma once
static inline unsigned long long ps3b_timebase(void) { return 0; }
static inline unsigned long long ps3b_timebase_freq(void) { return 79800000ULL; }
