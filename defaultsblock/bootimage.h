//
// bootimage.h
//
// Booting a staged image with optional defaults — the one place that decides
// what happens when a loader is given an image and, perhaps, a defaults-string
// to put in it.
//
// Every loader faces the same three cases: no defaults-string to stamp, a
// string and an image with a 0x800 block to take it, or a string and an image
// without one. Only the last is interesting, and the answer is that the image
// boots anyway. The defaults block is an OPTIONAL ABI: a plain Circle kernel
// carries none, and a loader that refuses such an image refuses most of what
// it could usefully boot. The refusal belongs to the WRITE -- the block is
// never stamped over startup code -- and never to the boot.
//
// Loaders call this instead of pairing PatchDefaults with EnableChainBoot
// themselves, so they cannot drift apart on that question, and so a loader
// written later inherits the answer rather than choosing it again.
//
#ifndef _bootimage_h
#define _bootimage_h

#include <circle/types.h>

// Stamp pDefaults into the staged image's 0x800 block when it has one, then
// arm the chain-boot. pDefaults may be 0 or empty, meaning boot it as it
// arrived. An image with no block, or a string too long for the block it has,
// boots unpatched -- never refused.
//
// Returns a one-line account of what happened, for a loader with a screen or
// a web page to show it on. The same account is written to the log, so a
// caller with neither can ignore the return value.
const char *BootImageWithDefaults (u8 *pImage, size_t nImageSize,
				   const char *pDefaults);

#endif
