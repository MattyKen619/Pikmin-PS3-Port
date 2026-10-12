// PS3 port: definitions for code the game never runs but the PS3 build still links.
//
// Game code is compiled with -finstrument-functions (thread-switch points, see os_sched.cpp). Each
// instrumented function loads its own address through its object file's TOC, and the linker keeps the
// whole TOC of an object once any of its functions is used, so unused functions next to used ones stay
// linked, together with what they reference. These are such references: they have no definition in the
// GameCube release (the matching build's linker discarded their callers).

#include "Actor.h"
#include "ps3/stub.h"

// Only ActorMgr's constructor uses this; ActorMgr is not used by the game (policeAI.cpp is a
// DLL-only translation unit).
PoliceAI::PoliceAI()
{
	PS3_STUB("PoliceAI::PoliceAI (unused code)");
}
