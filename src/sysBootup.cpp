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
#endif
	gsys->Initialise();
	nodeMgr = new NodeMgr();
	gsys->run(new PlugPikiApp());

	OSErrorLine(29, "End of demo");
}
