//
// rapi_chainboot.cpp
//
// Chain-boot for the Raspberry Pi 5, replacing Circle's implementation via
// link-level interposition: defining EnableChainBoot / IsChainBootEnabled /
// DoChainBoot here keeps libcircle's chainboot.o out of the link entirely
// (an archive member is only pulled for an otherwise-undefined symbol), so
// Circle is not modified. Pi 3/4 builds compile this file to nothing and
// keep Circle's own chain-boot.
//
// Circle's stock path cannot work on the Pi 5 (BCM2712 / Cortex-A76):
//
//  1. sysinit()'s CMemorySystem::Destructor() clears SCTLR_EL1.C and then
//     runs CleanDataCache() — compiled, stack-using C++ executing
//     non-cacheably while its own stack is dirty in the cache. The A76
//     does not tolerate that order. The hand-off below cleans FIRST,
//     while execution is still coherent, and the MMU/cache-off step is
//     registers-only assembly.
//
//  2. Circle stages its copy stub at MEM_KERNEL_START - 0x400 = 0x7FC00,
//     which on the Pi 5 is inside Trusted Firmware: TF-A's rpi5 platform
//     places BL31 at 0x1000..0x80000, and it stays resident to service
//     PSCI (the payload's secondary cores come up via CPU_ON). The stub
//     is staged in the loader's heap instead — above MEM_KERNEL_END,
//     clear of BL31 and of the copy destination.
//
//  3. The BCM2712 has a memory-side system cache that set/way maintenance
//     does not reach (set/way only maintains the PE's own architected
//     caches). Every line the loader leaves there is stale poison for a
//     payload that writes its early state (page tables, stacks, heap
//     headers) with the MMU off — straight to DRAM — and reads it back
//     with the MMU on, through that cache. VA maintenance IS honored by
//     system caches: the hand-off VA-cleans the stub and the staged
//     image, and the stub evicts the whole first gigabyte after the
//     copy. (The BCM2711 has no system cache, so Circle's set/way clean
//     suffices on the Pi 4.)
//
// Sequence: a boot server arms the chain-boot; CKernel::Run() returns;
// main() returns, running every ~CKernel device destructor (xHCI/macb
// quiesced and torn down); sysinit() then consults IsChainBootEnabled()
// before its own teardown — and ours, seeing main() has returned, performs
// the hand-off right there and never returns. The payload is entered at
// EL2 with the MMU off, the state a firmware boot hands it: the Pi 5
// firmware enters Circle at EL2, so the `hvc #0` return-to-EL2 vector
// (Circle's HVCStub) is installed and live.
//
#include "rapi_chainboot.h"

#include <circle/chainboot.h>
#include <circle/interrupt.h>
#include <circle/memory.h>
#include <circle/synchronize.h>
#include <circle/sysconfig.h>
#include <circle/macros.h>
#include <circle/util.h>
#include <circle/types.h>

#if RASPPI >= 5

static const void *s_pKernelImage = 0;
static size_t s_nKernelSize = 0;
static u8 *s_pStub = 0;

// Set through RapiChainBootMainReturning(), which a rider calls just before
// its CKernel::Run() returns ShutdownReboot: from then on
// IsChainBootEnabled()'s caller is sysinit(), after device teardown.
static volatile bool s_bLoaderMainReturned = false;

#define RAPI_STUB_MAX_SIZE	0x400

// Copy the staged payload over the running kernel, evict the loader's
// system-cache footprint, enter the payload. Runs at EL2 with the MMU off
// and must be self-contained — no stack, no calls out — because it
// overwrites the image it was compiled into.
static void RapiChainBootStub (const void *pKernelImage, size_t nKernelSize) MAXOPT;
static void RapiChainBootStub (const void *pKernelImage, size_t nKernelSize)
{
	const u32 *pSrc = (const u32 *) pKernelImage;
	u32 *pDest = (u32 *) MEM_KERNEL_START;

	nKernelSize += sizeof (u32)-1;
	nKernelSize /= sizeof (u32);

	while (nKernelSize--)
	{
		*pDest++ = *pSrc++;
	}

	// Evict the first gigabyte from the system cache (see header, 3.):
	// the payload's fixed regions and low heap all live here.
	for (uintptr nAddr = MEM_KERNEL_START; nAddr < GIGABYTE; nAddr += 64)
	{
		asm volatile ("dc civac, %0" : : "r" (nAddr) : "memory");
	}
	DataSyncBarrier ();

	InvalidateInstructionCache ();
	DataSyncBarrier ();
	InstructionSyncBarrier ();

	typedef void TKernelStart (void);
	(*(TKernelStart *) MEM_KERNEL_START) ();
}

void EnableChainBoot (const void *pKernelImage, size_t nKernelSize)
{
	s_pKernelImage = pKernelImage;
	s_nKernelSize = nKernelSize;

	if (s_pStub == 0)
	{
		// Heap chunk, cache-line aligned: above MEM_KERNEL_END, so it
		// survives the copy to MEM_KERNEL_START and is clear of BL31.
		s_pStub = (u8 *) (((uintptr) new u8[RAPI_STUB_MAX_SIZE + 63] + 63) & ~(uintptr) 63);
	}
	memcpy (s_pStub, (const void *) &RapiChainBootStub, RAPI_STUB_MAX_SIZE);

	InvalidateInstructionCache ();
	DataSyncBarrier ();
	InstructionSyncBarrier ();
}

boolean IsChainBootEnabled (void)
{
	if (s_pKernelImage == 0)
	{
		return FALSE;
	}

	if (!s_bLoaderMainReturned)
	{
		return TRUE;	// CKernel::Run()'s arming poll
	}

	// sysinit() is asking: device teardown is complete. Hand off now,
	// before sysinit()'s own (A76-unsafe) teardown; never returns.
	DoChainBoot ();

	return TRUE;	// not reached
}

void DoChainBoot (void)
{
	CInterruptSystem::Get ()->Destructor ();
	asm volatile ("msr DAIFSet, #0xf" ::: "memory");

	// Make RAM authoritative while execution is still coherent: set/way
	// clean for the architected caches, VA cleans for what must be
	// readable with the MMU off — the stub (fetched at EL2) and the
	// staged image (read by the stub's copy).
	CleanDataCache ();
	DataSyncBarrier ();
	CleanDataCacheRange ((u64) (uintptr) s_pStub, RAPI_STUB_MAX_SIZE);
	CleanDataCacheRange ((u64) (uintptr) s_pKernelImage, s_nKernelSize);
	DataSyncBarrier ();
	InvalidateInstructionCache ();
	DataSyncBarrier ();
	InstructionSyncBarrier ();

	// Registers only from here: EL1 MMU+D-cache off, TLB invalidated,
	// hvc to EL2, jump to the stub. Circle's HVCStub clobbers x0/x1, so
	// the stub's arguments ride x19-x21.
	asm volatile (
		"mov x19, %0\n"
		"mov x20, %1\n"
		"mov x21, %2\n"
		"mrs x3, sctlr_el1\n"
		"bic x3, x3, #(1 << 2)\n"	// SCTLR_EL1.C
		"bic x3, x3, #(1 << 0)\n"	// SCTLR_EL1.M
		"msr sctlr_el1, x3\n"
		"tlbi vmalle1\n"
		"dsb sy\n"
		"isb\n"
		"hvc #0\n"
		"mov x0, x19\n"
		"mov x1, x20\n"
		"br x21\n"
		:: "r" ((uintptr) s_pKernelImage),
		   "r" ((uintptr) s_nKernelSize),
		   "r" ((uintptr) s_pStub)
		: "x0", "x1", "x3", "x19", "x20", "x21", "memory");

	__builtin_unreachable ();
}

void RapiChainBootMainReturning (void)
{
	s_bLoaderMainReturned = true;
}

#else

// The Pi 3 and Pi 4 run Circle's own chain-boot, which needs no hand-off
// signal. The riders call this unconditionally all the same, so neither
// carries a board test of its own.
void RapiChainBootMainReturning (void)
{
}

#endif	// RASPPI >= 5
