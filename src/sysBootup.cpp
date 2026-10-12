#include "App.h"
#include "sysNew.h"
#include "system.h"
#if defined(TARGET_PS3)
#include "ps3/platform.h"
#endif

/**
 * @todo: Documentation
 */
TERNARY_BUILD_MATCHING(void, int) main(int argc, char* argv[])
{
#if defined(TARGET_PS3)
	ps3PlatformInit();
	ps3BootStage("game: System::Initialise");
#endif
	gsys->Initialise();
#if defined(TARGET_PS3)
	ps3BootStage("game: creating NodeMgr");
#endif
	nodeMgr = new NodeMgr();
#if defined(TARGET_PS3)
	ps3BootStage("game: creating PlugPikiApp and running it");
#endif
	gsys->run(new PlugPikiApp());

	OSErrorLine(29, "End of demo");
}
