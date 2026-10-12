#ifndef _PS3_SDK_INTERNAL_H
#define _PS3_SDK_INTERNAL_H

// PS3 port: internals shared by the SDK replacement files (compiled with the game's headers).

#include "Dolphin/os.h"
#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- logging (TTY + log file + on-screen console)
void ps3Log(const char* fmt, ...);
void ps3LogOnce(int* flag, const char* fmt, ...); // logs only the first time for a given flag

// ---- scheduler (os_sched.cpp)
// The GameCube runs one thread at a time, highest priority first, and the game relies on that.
// PS3 threads are used, but only the holder of the "baton" runs game code.
void ps3SchedInit(void);

// Interrupts: work posted by background threads (video timer, I/O) or by SDK code that must run "later"
// like a GameCube interrupt handler. Delivered at the next scheduling point by the running thread.
typedef void (*Ps3InterruptFn)(void* arg);
void ps3PostInterrupt(Ps3InterruptFn fn, void* arg);
void ps3DeliverInterrupts(void); // call at SDK entry points; cheap when nothing is pending

// Blocking helpers for SDK code (must be called by the running thread, never from an interrupt).
void ps3WaitOn(void* key, const char* reason); // sleep until ps3WakeAll(key)
void ps3WakeAll(void* key);                      // safe from interrupts
int ps3InInterrupt(void);

// Diagnostics
void ps3NoteActivity(const char* what); // remembers the last SDK call, for stall reports

// Use at the top of SDK functions: logs "[trace] first call: NAME" once, then notes the activity.
// The trace shows how far the game got if it stops without an error message.
#define PS3_ACTIVITY(name)                                     \
	do {                                                       \
		static int ps3_seen_;                                  \
		if (!ps3_seen_) {                                      \
			ps3_seen_ = 1;                                     \
			ps3Log("[trace] first call: %s\n", (name));        \
		}                                                      \
		ps3NoteActivity(name);                                 \
	} while (0)
unsigned int ps3FrameCounter(void);

// ---- other subsystems
void ps3DvdInit(void);
void ps3ViStart(void);

#ifdef __cplusplus
}
#endif

#endif // _PS3_SDK_INTERNAL_H
