// PS3 port: the -finstrument-functions hooks, compiled into each game source file (the Makefile
// force-includes this header into game code; platform/ps3/sdk/os_sched.cpp keeps the state).
//
// Every game function calls __cyg_profile_func_enter on entry and __cyg_profile_func_exit on return. The
// hooks keep a shadow call stack for stall reports and the profiler, record where the thread is
// (lastPc), and are the points where queued GameCube interrupts run and threads switch (see "preemption
// points" in os_sched.cpp).
//
// Up to test build 35 they were functions in os_sched.cpp. The program has several TOCs, so nearly every
// call to them went through a linker stub that saves and restores r2, whose store and reload of the same
// stack slot stall the PPU's pipeline (a load-hit-store) on every call. Defined here, static in each file,
// the compiler inlines them into the functions themselves: no call at all. Test builds up to 35 also
// called a hook at every basic block (-fsanitize-coverage=trace-pc), three times as often; lastPc now
// comes from these hooks: a function's entry, or the call site it returned to.

#ifndef PS3_HOOKS_H
#define PS3_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

#define PS3_SHADOW_DEPTH 128

struct Ps3HookState {
	void* lastPc;             // the code the thread ran last (a function's entry, or where it returned to)
	int depth;                // shadow call stack: entries in use (may exceed PS3_SHADOW_DEPTH)
	int pad;
	unsigned long long calls; // function entries (the watchdog tells a busy thread from a stopped one)
	void* stackFn[PS3_SHADOW_DEPTH];   // function descriptors
	void* stackSite[PS3_SHADOW_DEPTH]; // the call sites they return to
};

// The baton holder's state. The scheduler keeps each thread's own copy while it does not hold the baton
// and swaps it in and out here (os_sched.cpp: switchHooks). A fixed object, not a pointer to the
// thread's: in a function that calls nothing the compiler can then keep depth in a register from the entry
// hook to the exit hook, instead of storing it and loading it straight back (which stalls the PPU). Not
// volatile for the same reason; the baton changes hands only inside calls, and the threads that look at it
// from outside (watchdog, profiler) only read it.
extern struct Ps3HookState ps3Hook;
// Nonzero while GameCube interrupts are queued (set by the threads that post them).
extern volatile unsigned int ps3HookPending;
// Runs the queued interrupts, if they may run now, on the baton holder (pc: where it was).
void ps3HookDeliver(void* pc) __attribute__((no_instrument_function));

#ifndef PS3_HOOKS_NO_DEFINITIONS

static inline void __cyg_profile_func_enter(void* fn, void* site) __attribute__((no_instrument_function, unused));
static inline void __cyg_profile_func_exit(void* fn, void* site) __attribute__((no_instrument_function, unused));

static inline void __cyg_profile_func_enter(void* fn, void* site)
{
	const int d = ps3Hook.depth;
	void* pc    = *(void* const*)fn; // (fn is the function's descriptor: its first word is the code address)
	if (d < PS3_SHADOW_DEPTH) {
		ps3Hook.stackFn[d]   = fn;
		ps3Hook.stackSite[d] = site;
	}
	ps3Hook.depth  = d + 1;
	ps3Hook.calls  = ps3Hook.calls + 1;
	ps3Hook.lastPc = pc;
}

// (Queued interrupts run here, as the function returns: checked on entry, the call that runs them would make
// the compiler set up a stack frame in every function that calls nothing, and load depth back from memory
// in the exit hook.)
static inline void __cyg_profile_func_exit(void* fn, void* site)
{
	(void)fn;
	const int d = ps3Hook.depth;
	if (d > 0) {
		ps3Hook.depth = d - 1;
	}
	ps3Hook.lastPc = site;
	if (__builtin_expect(ps3HookPending != 0, 0)) {
		ps3HookDeliver(site);
	}
}

#endif // PS3_HOOKS_NO_DEFINITIONS

#ifdef __cplusplus
}
#endif

#endif
