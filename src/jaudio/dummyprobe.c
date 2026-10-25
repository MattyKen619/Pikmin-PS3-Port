#include "jaudio/dummyprobe.h"

#if defined(TARGET_PS3)
// PS3 port (test build 45): the probes time JAudio's work (platform/ps3/sdk/audio.cpp, ps3AudioProbe; the
// heartbeat logs them). On the GameCube they did nothing in the released game.
extern "C" void ps3AudioProbe(int id, const char* label, int start);

void Probe_Start(s32 id, immut char* label) { ps3AudioProbe(id, label, 1); }

void Probe_Finish(s32 id) { ps3AudioProbe(id, 0, 0); }
#else
/**
 * @TODO: Documentation
 */
void Probe_Start(s32 id, immut char* label)
{
}

/**
 * @TODO: Documentation
 */
void Probe_Finish(s32 id)
{
}
#endif

/**
 * @TODO: Documentation
 * @note UNUSED Size: 000050 (Matching by size)
 */
void Console_printf(immut char*, ...)
{
}
