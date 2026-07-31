//
// netconfig.cpp
//
// See netconfig.h for what this reads and why it lives in config.txt.
//
#include "netconfig.h"
#include <circle/logger.h>
#include <circle/util.h>
#include <fatfs/ff.h>

static const char FromNetConfig[] = "netconfig";

#define CONFIG_PATH		"SD:/config.txt"
#define CONFIG_SECTION		"rapi-bootloader"

// config.txt is a small text file; this is far more than the firmware itself
// will read, and a longer one simply has its tail ignored.
#define CONFIG_MAX_BYTES	8192

static boolean IsSpace (char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Compare two strings, ignoring case and ignoring surrounding whitespace in
// the first. Section and key names in config.txt are not case-sensitive.
static boolean MatchToken (const char *pStart, const char *pEnd, const char *pName)
{
	while (pStart < pEnd && IsSpace (*pStart))
	{
		pStart++;
	}

	while (pEnd > pStart && IsSpace (pEnd[-1]))
	{
		pEnd--;
	}

	for (; pStart < pEnd; pStart++, pName++)
	{
		if (*pName == '\0')
		{
			return FALSE;
		}

		char a = *pStart;
		char b = *pName;
		if ('A' <= a && a <= 'Z')	a += 'a' - 'A';
		if ('A' <= b && b <= 'Z')	b += 'a' - 'A';

		if (a != b)
		{
			return FALSE;
		}
	}

	return *pName == '\0';
}

// Parse a dotted-quad address. Returns FALSE on anything that is not four
// numbers of 0..255 separated by dots, so a typo is ignored rather than
// turned into some other machine's address.
static boolean ParseAddress (const char *pStart, const char *pEnd, u8 Address[4])
{
	while (pStart < pEnd && IsSpace (*pStart))
	{
		pStart++;
	}

	while (pEnd > pStart && IsSpace (pEnd[-1]))
	{
		pEnd--;
	}

	for (unsigned nOctet = 0; nOctet < 4; nOctet++)
	{
		if (pStart >= pEnd || *pStart < '0' || *pStart > '9')
		{
			return FALSE;
		}

		unsigned nValue = 0;
		unsigned nDigits = 0;
		while (pStart < pEnd && '0' <= *pStart && *pStart <= '9')
		{
			nValue = nValue * 10 + (unsigned) (*pStart++ - '0');
			if (++nDigits > 3 || nValue > 255)
			{
				return FALSE;
			}
		}

		Address[nOctet] = (u8) nValue;

		if (nOctet < 3)
		{
			if (pStart >= pEnd || *pStart != '.')
			{
				return FALSE;
			}
			pStart++;
		}
	}

	return pStart == pEnd;
}

void RapiReadNetConfig (TRapiNetConfig *pConfig)
{
	pConfig->bStatic = FALSE;
	pConfig->bHaveGateway = FALSE;

	// A netmask is optional; this is the one every address a home or office
	// network hands out uses, and stating it is still allowed.
	pConfig->NetMask[0] = 255;
	pConfig->NetMask[1] = 255;
	pConfig->NetMask[2] = 255;
	pConfig->NetMask[3] = 0;

	FIL File;
	if (f_open (&File, CONFIG_PATH, FA_READ) != FR_OK)
	{
		CLogger::Get ()->Write (FromNetConfig, LogNotice,
					"no " CONFIG_PATH " -- asking DHCP");
		return;
	}

	static char Buffer[CONFIG_MAX_BYTES];
	UINT nRead = 0;
	FRESULT Result = f_read (&File, Buffer, sizeof Buffer - 1, &nRead);
	f_close (&File);

	if (Result != FR_OK)
	{
		CLogger::Get ()->Write (FromNetConfig, LogWarning,
					"cannot read " CONFIG_PATH " -- asking DHCP");
		return;
	}

	Buffer[nRead] = '\0';

	boolean bInSection = FALSE;

	char *p = Buffer;
	while (*p != '\0')
	{
		char *pLine = p;
		while (*p != '\0' && *p != '\n')
		{
			p++;
		}
		char *pLineEnd = p;
		if (*p != '\0')
		{
			p++;
		}

		// Trim, then drop blank lines and comments.
		while (pLine < pLineEnd && IsSpace (*pLine))
		{
			pLine++;
		}
		while (pLineEnd > pLine && IsSpace (pLineEnd[-1]))
		{
			pLineEnd--;
		}
		if (pLine == pLineEnd || *pLine == '#')
		{
			continue;
		}

		// [section]
		if (*pLine == '[')
		{
			bInSection = FALSE;
			if (pLineEnd[-1] == ']')
			{
				bInSection = MatchToken (pLine + 1, pLineEnd - 1,
							 CONFIG_SECTION);
			}
			continue;
		}

		if (!bInSection)
		{
			continue;
		}

		char *pEquals = pLine;
		while (pEquals < pLineEnd && *pEquals != '=')
		{
			pEquals++;
		}
		if (pEquals == pLineEnd)
		{
			continue;
		}

		const char *pValue = pEquals + 1;

		if (MatchToken (pLine, pEquals, "ipaddress"))
		{
			if (ParseAddress (pValue, pLineEnd, pConfig->IPAddress))
			{
				pConfig->bStatic = TRUE;
			}
			else
			{
				CLogger::Get ()->Write (FromNetConfig, LogWarning,
							"ipaddress is not an address -- ignored");
			}
		}
		else if (MatchToken (pLine, pEquals, "netmask"))
		{
			if (!ParseAddress (pValue, pLineEnd, pConfig->NetMask))
			{
				CLogger::Get ()->Write (FromNetConfig, LogWarning,
							"netmask is not an address -- ignored");
			}
		}
		else if (MatchToken (pLine, pEquals, "gateway"))
		{
			if (ParseAddress (pValue, pLineEnd, pConfig->Gateway))
			{
				pConfig->bHaveGateway = TRUE;
			}
			else
			{
				CLogger::Get ()->Write (FromNetConfig, LogWarning,
							"gateway is not an address -- ignored");
			}
		}
	}

	if (!pConfig->bStatic)
	{
		CLogger::Get ()->Write (FromNetConfig, LogNotice,
					"no address in [" CONFIG_SECTION "] -- asking DHCP");
	}
}
