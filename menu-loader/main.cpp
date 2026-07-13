//
// main.cpp
//
// pi-mame boot picker entry point. Returning EXIT_REBOOT after arming
// chain-boot hands control to the staged, patched platform kernel image
// (Circle's reboot path calls DoChainBoot when EnableChainBoot() was used).
//
#include "kernel.h"
#include <circle/startup.h>

int main (void)
{
	CKernel Kernel;
	if (!Kernel.Initialize ())
	{
		return EXIT_HALT;
	}

	TShutdownMode ShutdownMode = Kernel.Run ();

	switch (ShutdownMode)
	{
	case ShutdownReboot:
		return EXIT_REBOOT;

	case ShutdownHalt:
	default:
		return EXIT_HALT;
	}
}
