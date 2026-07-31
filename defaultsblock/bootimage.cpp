//
// bootimage.cpp
//
// See bootimage.h.
//
#include "bootimage.h"
#include "defaultsblock.h"
#include <circle/chainboot.h>
#include <circle/logger.h>
#include <assert.h>

static const char FromBootImage[] = "bootimage";

const char *BootImageWithDefaults (u8 *pImage, size_t nImageSize,
				   const char *pDefaults)
{
	assert (pImage != 0);
	assert (nImageSize > 0);

	const char *pMessage = "booting";

	// Nothing to stamp is the quiet case, and by far the common one: say
	// nothing and boot the image as it arrived.
	if (   pDefaults != 0
	    && *pDefaults != '\0')
	{
		TPatchResult Result = PatchDefaults (pImage, nImageSize, pDefaults);

		if (Result == PatchOK)
		{
			pMessage = "defaults stamped, booting";
		}
		else
		{
			// Why the write was refused matters to whoever wrote
			// the string; that the image still boots matters more.
			pMessage = Result == PatchTooLong
				 ? "defaults too long for this image, booting unpatched"
				 : "image takes no defaults, booting unpatched";
		}

		CLogger::Get ()->Write (FromBootImage,
					Result == PatchOK ? LogNotice : LogWarning,
					"%s", pMessage);
	}

	EnableChainBoot (pImage, nImageSize);

	return pMessage;
}
