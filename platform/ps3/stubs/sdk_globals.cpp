// PS3 port: definitions for SDK globals that the game reads or writes directly.
// On the GameCube these live at fixed hardware addresses; here they are ordinary variables.
#include "Dolphin/GX/GXFifo.h"
#include "Dolphin/OS/OSAlloc.h"
#include "Dolphin/OS/OSTime.h"
#include "Dolphin/hw_regs.h"

extern "C" {

// Graphics write-gather FIFO. The inline GXPosition*/GXColor*/GXTexCoord* functions store vertex data
// here. Until the GX->RSX layer replaces those inlines (Phase 4), the writes land in this dummy object.
volatile PPCWGPipe GXWGFifo;

// Audio DSP hardware registers, poked directly by jaudio. Replaced by the DSP HLE in Phase 5.
vu16 __DSPRegs[32];

// GameCube bus clock (162 MHz). OS_TIMER_CLOCK = bus / 4, used by all OSTicksTo*/OS*ToTicks macros,
// so OSGetTick()/OSGetTime() on PS3 must count at 40.5 MHz to keep game timing correct.
u32 __OSBusClock = 162000000;

// Current OS heap handle (set by OSSetCurrentHeap; -1 = none), read by jaudio.
volatile OSHeapHandle __OSCurrHeap = -1;

} // extern "C"
