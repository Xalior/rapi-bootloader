//
// stagealloc.h
//
// Shared chain-boot staging allocator for every rapi-bootloader loader.
// One place to get a large buffer that is safe to receive a kernel into and
// then chain-boot from, on any board (1 GB .. 8 GB).
//
#ifndef _stagealloc_h
#define _stagealloc_h

#include <circle/types.h>

// Allocate nSize bytes for a chain-boot staging buffer, placed clear of the
// MEM_KERNEL_START..MEM_KERNEL_END copy region so the eventual copy to
// 0x80000 can never overlap the source. Uses the high heap (>= 1 GB) when the
// board has one with room, else the low heap — which by the Circle memory map
// begins above MEM_KERNEL_END, so it is always clear of the copy region.
// Returns nullptr if the board cannot hold nSize.
void *StageAlloc (size_t nSize);

#endif
