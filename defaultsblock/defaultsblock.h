//
// defaultsblock.h
//
// The 0x800 defaults-block ABI: a fixed-offset block a platform kernel
// image carries so any pre-boot writer (build system, chainloader, boot
// picker) can stamp the machine's argv defaults into the image before it
// runs. The consuming kernel tokenises the text and appends it to its own
// argv; this file is the writer's side only. The reader lives in whatever
// kernel image consumes the block, not in this repository.
//
// Layout (little-endian, aarch64 image loads at 0x80000):
//
//   image offset 0x800  (runtime address 0x80800)
//   +0x00  char  Magic[4]   'P','M','8','D' — seatbelt, verified before any
//                           write. Refuse the write if absent (a reordered
//                           link script becomes a refused write, never text
//                           stamped into startup code).
//   +0x04  u16   Capacity   bytes available in Text[] (512 to start). Lets
//                           the buffer grow without tool lockstep.
//   +0x06  u16   Length     bytes used in Text[], excluding the NUL.
//   +0x08  char  Text[512]  NUL-terminated plain-text argv string.
//
// The 8-byte header + 0x800 offset are deliberate: Text[] begins at image
// offset 0x808.
//
#ifndef _defaultsblock_h
#define _defaultsblock_h

#include <circle/types.h>
#include <circle/macros.h>

// Image offset (not runtime address) of the block.
#define DEFAULTS_BLOCK_OFFSET	0x800

// The seatbelt magic. Verified at offset before any writer touches a byte.
#define DEFAULTS_MAGIC0		'P'
#define DEFAULTS_MAGIC1		'M'
#define DEFAULTS_MAGIC2		'8'
#define DEFAULTS_MAGIC3		'D'

// The buffer size the first ABI revision ships. The authoritative capacity
// for any given image is the block's own Capacity field — a writer never
// assumes this constant, it reads Capacity and enforces against it.
#define DEFAULTS_BUFFER_BYTES	512

struct TDefaultsBlock
{
	char	Magic[4];			// 'P','M','8','D'
	u16	Capacity;			// bytes available in Text[]
	u16	Length;				// bytes used in Text[], excluding NUL
	char	Text[DEFAULTS_BUFFER_BYTES];	// NUL-terminated argv string
}
PACKED;

enum TPatchResult
{
	PatchOK,		// magic verified, string fit, written
	PatchImageTooSmall,	// image cannot contain the block header
	PatchNoMagic,		// magic absent at offset — write refused (seatbelt)
	PatchTooLong		// string longer than the block's Capacity allows
};

// Verify the magic at DEFAULTS_BLOCK_OFFSET inside a staged kernel image,
// enforce the string length against the block's own Capacity field, and —
// only if both hold — write the NUL-terminated string into Text[] and update
// Length. The buffer never trusts its writer: an absent magic or an
// over-long string is refused, nothing is written, and the reason returned.
TPatchResult PatchDefaults (u8 *pImage, size_t nImageSize, const char *pString);

// Human-readable form of a TPatchResult, for logging.
const char *PatchResultString (TPatchResult Result);

#endif
