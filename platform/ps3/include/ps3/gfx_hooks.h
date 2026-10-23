// PS3 port: -finstrument-functions hooks for the graphics thread's code (the command processor, the
// renderer and the RSX device: Makefile.ps3, GFX_PROFILE), for the sampling profiler (profiler.cpp).
//
// Like the game's hooks (hooks.h), they are compiled into each file so the compiler inlines them. They only
// note where the graphics thread is: the last function it entered or the call site it returned to (self
// time), and a shadow call stack (inclusive time). The profiler looks at them while the graphics thread is
// busy with commands (ps3GfxPhase is not idle). Only the graphics thread runs this code once the game is
// going (start-up and status reports call into it from other threads now and then: a sample or two may be
// off).

#ifndef PS3_GFX_HOOKS_H
#define PS3_GFX_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_GFX_SHADOW_DEPTH 32

struct Ps3GfxHookState {
	void* lastPc; // the code the graphics thread ran last (a function's entry, or where it returned to)
	int depth;    // shadow call stack: entries in use (may exceed PS3_GFX_SHADOW_DEPTH)
	int pad;
	void* stackFn[PS3_GFX_SHADOW_DEPTH]; // function descriptors
};

// (Not volatile: the profiler only reads it, and a sample may be slightly out of date.)
extern struct Ps3GfxHookState ps3GfxHook;

#ifndef PS3_GFX_HOOKS_NO_DEFINITIONS

static inline void __cyg_profile_func_enter(void* fn, void* site) __attribute__((no_instrument_function, unused));
static inline void __cyg_profile_func_exit(void* fn, void* site) __attribute__((no_instrument_function, unused));

static inline void __cyg_profile_func_enter(void* fn, void* site)
{
	(void)site;
	const int d = ps3GfxHook.depth;
	if ((unsigned)d < PS3_GFX_SHADOW_DEPTH) {
		ps3GfxHook.stackFn[d] = fn;
	}
	ps3GfxHook.depth  = d + 1;
	ps3GfxHook.lastPc = *(void* const*)fn; // (fn is the function's descriptor: its first word is the code address)
}

static inline void __cyg_profile_func_exit(void* fn, void* site)
{
	(void)fn;
	const int d = ps3GfxHook.depth;
	ps3GfxHook.depth  = d > 0 ? d - 1 : 0;
	ps3GfxHook.lastPc = site;
}

#endif // PS3_GFX_HOOKS_NO_DEFINITIONS

#ifdef __cplusplus
}
#endif

#endif
