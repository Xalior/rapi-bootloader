//
// rapi_chainboot.h
//
// Shared chain-boot for every rapi-bootloader loader. Both riders — the
// menu-loader on the card and the network-loader on the bench — hand a
// staged payload over through this one implementation.
//
// On the Pi 5 (BCM2712 / Cortex-A76) Circle's own chain-boot cannot work,
// so rapi_chainboot.cpp replaces it by link-level interposition; on the
// Pi 3 and Pi 4 Circle's implementation is correct and is what runs. A
// rider needs to know none of that: it arms the boot with Circle's
// EnableChainBoot() as always, and calls the one function below.
//
#ifndef _rapi_chainboot_h
#define _rapi_chainboot_h

// Call this from CKernel::Run() immediately before it returns
// ShutdownReboot, after the loader's own device settling is done.
//
// It marks the point where the loader's main() begins to return, which is
// what lets the Pi 5 hand-off distinguish its two callers: the arming poll
// inside Run(), and sysinit() asking after the full ~CKernel device
// teardown. The Pi 5 hand-off happens on the second, while execution is
// still coherent and before sysinit()'s own teardown — which the A76 does
// not survive.
//
// On boards that use Circle's chain-boot this does nothing, so riders call
// it unconditionally and carry no board test of their own.
void RapiChainBootMainReturning (void);

#endif
