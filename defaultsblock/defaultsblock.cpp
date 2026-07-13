//
// defaultsblock.cpp
//
// The writer side of the pi-mame patchable-defaults ABI. See defaultsblock.h
// for the block layout and the seatbelt contract.
//
#include "defaultsblock.h"
#include <circle/util.h>

TPatchResult PatchDefaults (u8 *pImage, size_t nImageSize, const char *pString)
{
	// The image must be large enough to contain the fixed-size block header
	// and its buffer at the ABI offset before we may dereference it.
	if (nImageSize < DEFAULTS_BLOCK_OFFSET + sizeof (TDefaultsBlock))
	{
		return PatchImageTooSmall;
	}

	TDefaultsBlock *pBlock =
		(TDefaultsBlock *) (pImage + DEFAULTS_BLOCK_OFFSET);

	// Seatbelt: verify the magic at the fixed offset BEFORE writing a byte.
	// A reordered link script (block no longer here) refuses the write
	// rather than stamping argv text over startup code.
	if (   pBlock->Magic[0] != DEFAULTS_MAGIC0
	    || pBlock->Magic[1] != DEFAULTS_MAGIC1
	    || pBlock->Magic[2] != DEFAULTS_MAGIC2
	    || pBlock->Magic[3] != DEFAULTS_MAGIC3)
	{
		return PatchNoMagic;
	}

	// The block declares its own capacity; enforce the string length against
	// it (room for the trailing NUL). The buffer never trusts its writer.
	size_t nCapacity = pBlock->Capacity;
	size_t nLength = strlen (pString);
	if (nLength + 1 > nCapacity)
	{
		return PatchTooLong;
	}

	memcpy (pBlock->Text, pString, nLength);
	pBlock->Text[nLength] = '\0';
	pBlock->Length = (u16) nLength;

	return PatchOK;
}

const char *PatchResultString (TPatchResult Result)
{
	switch (Result)
	{
	case PatchOK:		return "ok";
	case PatchImageTooSmall:return "platform image too small to hold defaults block";
	case PatchNoMagic:	return "defaults magic absent at 0x800 - refusing to patch";
	case PatchTooLong:	return "defaults string exceeds block capacity";
	default:		return "unknown";
	}
}
