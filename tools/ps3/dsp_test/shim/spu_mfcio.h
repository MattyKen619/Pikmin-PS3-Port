// tools/ps3/dsp_test: the SPU's DMA on the host. Transfers are memcpy; each one is checked against the MFC's
// rules first (sizes, alignment, the same lowest 4 address bits) and against the memory the test set up, so a
// transfer that would stop the SPU on the console fails the test here.
#pragma once
#include <stdint.h>
#include <string.h>

void hostDmaCheck(const volatile void* ls, uint64_t ea, uint32_t size, const char* what);
uint32_t hostDecrementer(void);

static inline void mfc_get(volatile void* ls, uint64_t ea, uint32_t size, uint32_t tag, uint32_t tid, uint32_t rid)
{
	(void)tag, (void)tid, (void)rid;
	hostDmaCheck(ls, ea, size, "get");
	memcpy((void*)ls, (const void*)(uintptr_t)ea, size);
}

static inline void mfc_put(volatile void* ls, uint64_t ea, uint32_t size, uint32_t tag, uint32_t tid, uint32_t rid)
{
	(void)tag, (void)tid, (void)rid;
	hostDmaCheck(ls, ea, size, "put");
	memcpy((void*)(uintptr_t)ea, (const void*)ls, size);
}

static inline void hostGetllar(volatile void* ls, uint64_t ea)
{
	if (((uintptr_t)ls & 127) || (ea & 127)) {
		hostDmaCheck(ls, ea, 0, "getllar (not 128-byte aligned)");
	}
	hostDmaCheck(ls, ea, 128, "getllar");
	memcpy((void*)ls, (const void*)(uintptr_t)ea, 128);
}

#define mfc_getllar(ls, ea, tid, rid) hostGetllar((ls), (ea))
#define mfc_read_atomic_status()      0u
#define mfc_write_tag_mask(mask)      ((void)(mask))
#define mfc_read_tag_status_all()     0u
#define spu_read_decrementer()        hostDecrementer()
#define spu_write_decrementer(count)  ((void)(count))
#define spu_read_signal1()            0u
