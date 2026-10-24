// tools/ps3/dsp_test: stands in for PSL1GHT's SPU event call on the host.
#pragma once
#include <stdint.h>
static inline int spu_thread_send_event(uint8_t port, uint32_t data0, uint32_t data1)
{
	(void)port, (void)data0, (void)data1;
	return 0;
}
