//
// stagealloc.cpp
//
// See stagealloc.h.
//
// A large image is received into this buffer and later copied to
// MEM_KERNEL_START (0x80000) to chain-boot. The buffer must therefore not sit
// inside MEM_KERNEL_START..MEM_KERNEL_END, or the copy would overwrite its own
// source. Two placements satisfy that:
//   - the high heap (MEM_HIGHMEM_START = 1 GB and up) — Pi 4 / Pi 5 only;
//   - the low heap, which the Circle memory map starts at MEM_HEAP_START,
//     already above MEM_KERNEL_END (MEM_KERNEL_START + KERNEL_MAX_SIZE).
// A 1 GB board (Pi 3) has NO high heap — it is never Setup, so allocating from
// it corrupts the allocator. Hence the GetHighMemSize() gate before touching it.
//
#include "stagealloc.h"
#include <circle/memory.h>
#include <circle/new.h>

void *StageAlloc (size_t nSize)
{
	CMemorySystem *pMem = CMemorySystem::Get ();

	// High heap when the board actually has high memory with room. The
	// GetHighMemSize() test short-circuits so a 1 GB board never queries or
	// allocates from its (never-Setup) high heap.
	if (   CMemorySystem::GetHighMemSize () >= nSize
	    && pMem->GetHeapFreeSpace (HEAP_HIGH) >= nSize)
	{
		return new (HEAP_HIGH) u8[nSize];
	}

	// Low heap otherwise — always present and, by the memory map, above the
	// chain-boot copy region.
	if (pMem->GetHeapFreeSpace (HEAP_LOW) >= nSize)
	{
		return new (HEAP_LOW) u8[nSize];
	}

	return 0;
}
