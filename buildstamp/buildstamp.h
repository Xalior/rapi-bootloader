//
// buildstamp.h
//
// The image's own build time, as a string for a boot banner.
//
// A __DATE__ in an ordinary source file cannot be trusted for this: it
// updates only when ITS translation unit recompiles, so a relink that
// reused that object prints an old time for a new image — a fresh build
// then reads as "the wrong image was pushed". This string lives in its own
// tiny object, and the rider's Makefile makes that object depend on every
// other object and library in the link, so it recompiles exactly when the
// image's content changes and the banner names the build it rides in.
//
#ifndef _buildstamp_h
#define _buildstamp_h

extern const char RapiBuildStamp[];	// e.g. "Aug  6 2026 15:18:45"

#endif
