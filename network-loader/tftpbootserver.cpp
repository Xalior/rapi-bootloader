//
// tftpbootserver.cpp
//
// pi-mame chainloader TFTP server — see tftpbootserver.h for the protocol.
// Based on Circle sample/38-bootloader (GPLv3), R. Stange.
//
#include "tftpbootserver.h"
#include <circle/chainboot.h>
#include <circle/logger.h>
#include <circle/util.h>
#include <circle/new.h>
#include <circle/string.h>
#include <assert.h>

static const char FromBootServer[] = "tftpboot";

// Reserved TFTP write name (dev tooling): its bytes become the defaults-string
// injected into the next kernel push. Not a file — never touches the SD card.
static const char InjectFileName[] = "inject";

// "sd/<path>" on the wire -> "SD:/<path>" for FatFs; returns FALSE if not sd/
static boolean MapSDPath (const char *pFileName, CString *pPath)
{
	if (strncmp (pFileName, "sd/", 3) != 0 || pFileName[3] == '\0')
	{
		return FALSE;
	}

	pPath->Format ("SD:/%s", pFileName + 3);

	return TRUE;
}

CTFTPBootServer::CTFTPBootServer (CNetSubSystem *pNetSubSystem, size_t nMaxKernelSize)
:	CTFTPDaemon (pNetSubSystem),
	m_nMaxKernelSize (nMaxKernelSize),
	m_Mode (ModeNone),
	m_pKernelBuffer (0),
	m_bInjectPending (FALSE),
	m_nInjectFill (0),
	m_pWriteBuf (0),
	m_nWriteBufFill (0)
{
	m_InjectText[0] = '\0';
}

CTFTPBootServer::~CTFTPBootServer (void)
{
	assert (m_Mode == ModeNone);

	delete [] m_pKernelBuffer;
	m_pKernelBuffer = 0;

	delete [] m_pWriteBuf;
	m_pWriteBuf = 0;
}

boolean CTFTPBootServer::FlushWriteBuf (void)
{
	if (m_nWriteBufFill == 0)
	{
		return TRUE;
	}

	UINT nWritten = 0;
	FRESULT Result = f_write (&m_File, m_pWriteBuf, m_nWriteBufFill, &nWritten);
	boolean bOK = Result == FR_OK && nWritten == m_nWriteBufFill;

	m_nWriteBufFill = 0;

	return bOK;
}

boolean CTFTPBootServer::FileOpen (const char *pFileName)
{
	if (m_Mode != ModeNone)
	{
		return FALSE;
	}

	CString Path;
	if (!MapSDPath (pFileName, &Path))
	{
		return FALSE;		// only sd/ files are readable
	}

	if (f_open (&m_File, Path, FA_READ) != FR_OK)
	{
		CLogger::Get ()->Write (FromBootServer, LogWarning,
					"Cannot open %s", (const char *) Path);
		return FALSE;
	}

	CLogger::Get ()->Write (FromBootServer, LogDebug,
				"Sending %s ...", (const char *) Path);
	m_Mode = ModeSDRead;

	return TRUE;
}

boolean CTFTPBootServer::FileCreate (const char *pFileName)
{
	if (m_Mode != ModeNone)
	{
		return FALSE;
	}

	assert (pFileName != 0);

	// Defaults-string injection (dev tooling): the reserved "inject" name.
	// Its bytes are stashed and armed for the NEXT kernel push; nothing is
	// written to the SD card.
	if (strcmp (pFileName, InjectFileName) == 0)
	{
		m_nInjectFill = 0;
		m_InjectText[0] = '\0';
		m_Mode = ModeInject;

		return TRUE;
	}

	// SD card write: "sd/<path>" (parent directories are created on demand)
	CString Path;
	if (MapSDPath (pFileName, &Path))
	{
		const char *pPath = (const char *) Path;
		for (const char *p = pPath + 4 /* skip "SD:/" */; *p != '\0'; p++)
		{
			if (*p == '/')
			{
				char Dir[FF_MAX_LFN + 8];
				size_t nLen = (size_t) (p - pPath);
				if (nLen < sizeof Dir)
				{
					memcpy (Dir, pPath, nLen);
					Dir[nLen] = '\0';
					f_mkdir (Dir);	// FR_EXIST is fine
				}
			}
		}

		if (f_open (&m_File, Path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		{
			CLogger::Get ()->Write (FromBootServer, LogWarning,
						"Cannot create %s", (const char *) Path);
			return FALSE;
		}

		if (m_pWriteBuf == 0)
		{
			m_pWriteBuf = new (HEAP_HIGH) u8[WRITE_BUF_SIZE];
		}
		m_nWriteBufFill = 0;

		CLogger::Get ()->Write (FromBootServer, LogDebug,
					"Writing %s ...", (const char *) Path);
		m_Mode = ModeSDWrite;

		return TRUE;
	}

	// Chain-boot payload: "kernel*.img"
	static const char FileBaseName[] = "kernel";
	if (strncmp (pFileName, FileBaseName, sizeof FileBaseName-1) != 0)
	{
		return FALSE;
	}

	static const char FileExt[] = ".img";
	size_t nLen = strlen (pFileName);
	assert (nLen > sizeof FileExt);
	if (strcmp (&pFileName[nLen - (sizeof FileExt-1)], FileExt) != 0)
	{
		return FALSE;
	}

	CLogger::Get ()->Write (FromBootServer, LogDebug, "Receiving %s ...", pFileName);

	if (m_pKernelBuffer == 0)
	{
		// pi-mame: stage in the high heap (>1GB) so a large incoming image
		// can never overlap its own copy destination at MEM_KERNEL_START.
		m_pKernelBuffer = new (HEAP_HIGH) u8[m_nMaxKernelSize];
		if (m_pKernelBuffer == 0)
		{
			return FALSE;
		}
	}

	m_nCurrentOffset = 0;
	m_Mode = ModeKernel;

	return TRUE;
}

boolean CTFTPBootServer::FileClose (void)
{
	switch (m_Mode)
	{
	case ModeKernel:
		CLogger::Get ()->Write (FromBootServer, LogDebug,
					"%u bytes received", m_nCurrentOffset);
		if (m_nCurrentOffset > 0)
		{
			// Dev tooling: if an "inject" push armed a defaults-string,
			// stamp it into this image's 0x800 block before booting.
			// The write is magic-verified and length-enforced by the
			// shared PatchDefaults; a missing/short block is refused,
			// never stamped over startup code. One-shot: disarm after
			// the attempt so it never leaks to a later un-injected boot.
			if (m_bInjectPending)
			{
				TPatchResult Result = PatchDefaults (
					m_pKernelBuffer, m_nCurrentOffset, m_InjectText);
				CLogger::Get ()->Write (FromBootServer,
					Result == PatchOK ? LogNotice : LogWarning,
					"defaults injection: %s",
					PatchResultString (Result));

				m_bInjectPending = FALSE;
				m_nInjectFill = 0;
				m_InjectText[0] = '\0';
			}

			EnableChainBoot (m_pKernelBuffer, m_nCurrentOffset);
		}
		break;

	case ModeInject:
		// Trim a trailing newline (a shell "echo" convenience) so the
		// stamped argv string is exactly what the caller intended.
		while (   m_nInjectFill > 0
		       && (   m_InjectText[m_nInjectFill - 1] == '\n'
			   || m_InjectText[m_nInjectFill - 1] == '\r'))
		{
			m_nInjectFill--;
		}
		m_InjectText[m_nInjectFill] = '\0';
		m_bInjectPending = TRUE;
		CLogger::Get ()->Write (FromBootServer, LogNotice,
					"defaults armed for next kernel push: \"%s\"",
					m_InjectText);
		break;

	case ModeSDWrite:
	{
		FSIZE_t nReceived = f_tell (&m_File) + m_nWriteBufFill;
		if (!FlushWriteBuf ())
		{
			CLogger::Get ()->Write (FromBootServer, LogWarning,
						"Final flush failed");
		}
		f_close (&m_File);
		CLogger::Get ()->Write (FromBootServer, LogNotice,
					"File closed, %lu bytes on card",
					(unsigned long) nReceived);
		break;
	}

	case ModeSDRead:
		f_close (&m_File);
		CLogger::Get ()->Write (FromBootServer, LogDebug, "File closed");
		break;

	default:
		assert (0);
		break;
	}

	m_Mode = ModeNone;

	return TRUE;
}

int CTFTPBootServer::FileRead (void *pBuffer, unsigned nCount)
{
	if (m_Mode != ModeSDRead)
	{
		return -1;
	}

	UINT nRead = 0;
	if (f_read (&m_File, pBuffer, nCount, &nRead) != FR_OK)
	{
		return -1;
	}

	return (int) nRead;
}

int CTFTPBootServer::FileWrite (const void *pBuffer, unsigned nCount)
{
	switch (m_Mode)
	{
	case ModeKernel:
		if (m_nCurrentOffset + nCount > m_nMaxKernelSize)
		{
			m_Mode = ModeNone;
			return -1;
		}

		assert (pBuffer != 0);
		memcpy (m_pKernelBuffer + m_nCurrentOffset, pBuffer, nCount);
		m_nCurrentOffset += nCount;

		return nCount;

	case ModeInject:
		// Accumulate into the fixed defaults buffer, leaving room for the
		// NUL. An over-long injection is refused here (as it would be by
		// PatchDefaults against the block Capacity) rather than truncated.
		if (m_nInjectFill + nCount > sizeof m_InjectText - 1)
		{
			CLogger::Get ()->Write (FromBootServer, LogWarning,
				"injection string exceeds %u bytes -- refused",
				(unsigned) (sizeof m_InjectText - 1));
			m_Mode = ModeNone;
			return -1;
		}

		assert (pBuffer != 0);
		memcpy (m_InjectText + m_nInjectFill, pBuffer, nCount);
		m_nInjectFill += nCount;

		return (int) nCount;

	case ModeSDWrite:
		if (m_pWriteBuf == 0)
		{
			m_Mode = ModeNone;
			f_close (&m_File);
			return -1;
		}

		if (m_nWriteBufFill + nCount > WRITE_BUF_SIZE && !FlushWriteBuf ())
		{
			m_Mode = ModeNone;
			f_close (&m_File);
			return -1;
		}

		assert (pBuffer != 0);
		memcpy (m_pWriteBuf + m_nWriteBufFill, pBuffer, nCount);
		m_nWriteBufFill += nCount;

		return (int) nCount;

	default:
		return -1;
	}
}
