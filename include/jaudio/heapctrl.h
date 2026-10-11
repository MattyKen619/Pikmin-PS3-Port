#ifndef _JAUDIO_HEAPCTRL_H
#define _JAUDIO_HEAPCTRL_H

#include "types.h"

typedef struct jaheap_ jaheap_;

struct jaheap_ {
	u8 isRootHeap;             // _00, is this a 'mother' heap?
	u8 memoryType;             // _01, 0 = ARAM, 1 = DRAM
	u16 childCount;            // _02
	u32 heapId;                // _04
	u32 startAddress;          // _08
	u32 usedSize;              // _0C
	u32 size;                  // _10
	PTR32(jaheap_) firstChild;       // _14
	PTR32(jaheap_) parent;           // _18
	PTR32(jaheap_) nextSibling;      // _1C
	PTR32(jaheap_) groupOwner;       // _20
	PTR32(jaheap_) firstGroupedHeap; // _24
	PTR32(jaheap_) nextGroupedHeap;  // _28
};

void Jac_GetUnlockHeap(jaheap_*);
void Jac_CheckAlloc(jaheap_*);
void Jac_InitHeap(jaheap_*);
void Jac_SelfInitHeap(jaheap_*, u32, u32, u32);
BOOL Jac_SelfAllocHeap(jaheap_*, jaheap_*, u32, u32);
BOOL Jac_SetGroupHeap(jaheap_*, jaheap_*);
void Jac_CutdownHeap(jaheap_*);
void Jac_InitMotherHeap(jaheap_*, u32, u32, u8);
BOOL Jac_AllocHeap(jaheap_*, jaheap_*, u32);
BOOL Jac_DeleteHeap(jaheap_*);
void Jac_GarbageCollection_St(jaheap_*);
void Jac_CheckFreeHeap_Total(jaheap_*);
void Jac_CheckFreeHeap_Linear(jaheap_*);
void Jac_ShowHeap(jaheap_*, u32);

// jaheap_ is embedded in file data (WaveID_, WaveArchive_), so its size is fixed. See PTR32 in types.h.
#if defined(__cplusplus) && __cplusplus >= 201103L
static_assert(sizeof(jaheap_) == 0x2C, "jaheap_: file layout size changed");
static_assert(__builtin_offsetof(jaheap_, nextGroupedHeap) == 0x28, "jaheap_::nextGroupedHeap: file layout offset changed");
#endif

#endif
