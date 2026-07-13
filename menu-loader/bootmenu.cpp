//
// bootmenu.cpp
//
// See bootmenu.h. Reads the whole (small) config file into a heap buffer and
// parses it line by line in place.
//
#include "bootmenu.h"
#include <circle/logger.h>
#include <circle/util.h>
#include <circle/new.h>
#include <fatfs/ff.h>

static const char FromBootMenu[] = "bootmenu";

// The config is human-authored and tiny; refuse to read anything absurd.
static const unsigned MaxConfigBytes = 64 * 1024;

CBootMenu::CBootMenu (void)
:	m_nCount (0)
{
}

CBootMenu::~CBootMenu (void)
{
}

static boolean IsSpace (char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

void CBootMenu::ParseLine (const char *pLine, size_t nLen)
{
	// Trim leading whitespace.
	while (nLen > 0 && IsSpace (*pLine))
	{
		pLine++;
		nLen--;
	}

	// Trim trailing whitespace (notably the '\r' of CRLF files).
	while (nLen > 0 && IsSpace (pLine[nLen - 1]))
	{
		nLen--;
	}

	// Skip blank lines and comments.
	if (nLen == 0 || pLine[0] == '#')
	{
		return;
	}

	// Split on the FIRST '|'. A line without a pipe is malformed — skip it.
	size_t nPipe = 0;
	while (nPipe < nLen && pLine[nPipe] != '|')
	{
		nPipe++;
	}
	if (nPipe == nLen)
	{
		CLogger::Get ()->Write (FromBootMenu, LogWarning,
					"Ignoring line with no '|' separator");
		return;
	}

	if (m_nCount >= MaxEntries)
	{
		CLogger::Get ()->Write (FromBootMenu, LogWarning,
					"Entry limit (%u) reached; ignoring the rest",
					MaxEntries);
		return;
	}

	// Label: [start, nPipe), with trailing whitespace trimmed.
	size_t nLabelLen = nPipe;
	while (nLabelLen > 0 && IsSpace (pLine[nLabelLen - 1]))
	{
		nLabelLen--;
	}
	if (nLabelLen >= MaxLabel)
	{
		nLabelLen = MaxLabel - 1;
	}
	memcpy (m_Label[m_nCount], pLine, nLabelLen);
	m_Label[m_nCount][nLabelLen] = '\0';

	// String: (nPipe, nLen), with leading whitespace trimmed (trailing was
	// already trimmed off the whole line above).
	const char *pStr = pLine + nPipe + 1;
	size_t nStrLen = nLen - nPipe - 1;
	while (nStrLen > 0 && IsSpace (*pStr))
	{
		pStr++;
		nStrLen--;
	}
	if (nStrLen >= MaxString)
	{
		nStrLen = MaxString - 1;
	}
	memcpy (m_String[m_nCount], pStr, nStrLen);
	m_String[m_nCount][nStrLen] = '\0';

	m_nCount++;
}

boolean CBootMenu::Load (const char *pPath)
{
	m_nCount = 0;

	FIL File;
	if (f_open (&File, pPath, FA_READ) != FR_OK)
	{
		CLogger::Get ()->Write (FromBootMenu, LogWarning,
					"Cannot open %s", pPath);
		return FALSE;
	}

	FSIZE_t nSize = f_size (&File);
	if (nSize > MaxConfigBytes)
	{
		nSize = MaxConfigBytes;
	}

	char *pBuffer = new char[(size_t) nSize + 1];
	if (pBuffer == 0)
	{
		f_close (&File);
		return FALSE;
	}

	UINT nRead = 0;
	FRESULT Result = f_read (&File, pBuffer, (UINT) nSize, &nRead);
	f_close (&File);

	if (Result != FR_OK)
	{
		delete [] pBuffer;
		return FALSE;
	}
	pBuffer[nRead] = '\0';

	// Walk lines, splitting on '\n' (the '\r' of CRLF is trimmed per line).
	size_t nStart = 0;
	for (size_t i = 0; i <= nRead; i++)
	{
		if (i == nRead || pBuffer[i] == '\n')
		{
			ParseLine (pBuffer + nStart, i - nStart);
			nStart = i + 1;
		}
	}

	delete [] pBuffer;

	CLogger::Get ()->Write (FromBootMenu, LogNotice,
				"%s: %u entr%s", pPath, m_nCount,
				m_nCount == 1 ? "y" : "ies");

	return TRUE;
}

unsigned CBootMenu::GetCount (void) const
{
	return m_nCount;
}

const char *CBootMenu::GetLabel (unsigned nIndex) const
{
	return nIndex < m_nCount ? m_Label[nIndex] : 0;
}

const char *CBootMenu::GetString (unsigned nIndex) const
{
	return nIndex < m_nCount ? m_String[nIndex] : 0;
}
