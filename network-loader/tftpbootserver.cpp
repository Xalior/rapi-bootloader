//
// tftpbootserver.cpp
//
// network-loader's TFTP server — see tftpbootserver.h for the protocol.
// Based on Circle sample/38-bootloader (GPLv3), R. Stange.
//
#include "tftpbootserver.h"
#include "stagealloc.h"
#include "bootimage.h"
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
	m_bKernelClosed (FALSE),
	m_nKernelHash (2166136261U),
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
			m_pWriteBuf = (u8 *) StageAlloc (WRITE_BUF_SIZE);
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
		m_pKernelBuffer = (u8 *) StageAlloc (m_nMaxKernelSize);
		if (m_pKernelBuffer == 0)
		{
			return FALSE;
		}
	}

	m_nCurrentOffset = 0;
	m_nKernelHash = 2166136261U;		// FNV-1a offset basis
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
		CLogger::Get ()->Write (FromBootServer, LogNotice,
					"image fnv1a %08X over %u bytes",
					(unsigned) m_nKernelHash,
					(unsigned) m_nCurrentOffset);
		// Whether these bytes are a complete payload is not knowable
		// here: the daemon calls FileClose() for completed, timed-out
		// and aborted transfers alike. Hand the verdict to
		// UpdateStatus(), which the daemon calls next with the actual
		// transfer status — only a completed push may chain-boot.
		m_bKernelClosed = m_nCurrentOffset > 0;
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

void CTFTPBootServer::UpdateStatus (TStatus Status, const char *pFileName)
{
	if (!m_bKernelClosed)
	{
		return;
	}

	switch (Status)
	{
	case StatusWriteCompleted:
		m_bKernelClosed = FALSE;

		// A pending "inject" push supplies the defaults-string. The
		// shared writer stamps it when this image has a 0x800 block and
		// boots it either way. One-shot: disarmed after the attempt so
		// it never leaks into a later un-injected boot.
		BootImageWithDefaults (m_pKernelBuffer, m_nCurrentOffset,
				       m_bInjectPending ? m_InjectText : 0);

		m_bInjectPending = FALSE;
		m_nInjectFill = 0;
		m_InjectText[0] = '\0';
		break;

	case StatusWriteAborted:
		// A timed-out or aborted push: a truncated payload must never
		// boot. Discard it; a pending injection stays armed for the
		// caller's retry.
		m_bKernelClosed = FALSE;
		CLogger::Get ()->Write (FromBootServer, LogWarning,
					"truncated push discarded (%u bytes)",
					m_nCurrentOffset);
		m_nCurrentOffset = 0;
		break;

	default:
		break;
	}
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

		// Hashed HERE, one block at a time, and never in the completion
		// callback. TFTP rides UDP, so an image of the right length and
		// the wrong content is possible and would be indistinguishable
		// from a payload that simply does not run; the host can compute
		// the same value over the file it pushed.
		//
		// A single pass over the whole buffer at completion looks
		// cheaper to write and is not: it is a synchronous burst of a
		// million iterations inside a task callback, with no Yield, at
		// the exact moment the loader is about to quiesce xHCI. That
		// starved the USB pump and left one of the controller's two
		// shared-memory blocks unfreed at teardown. Per block, the cost
		// is a few thousand iterations between packets and nothing is
		// starved.
		{
			const u8 *p = (const u8 *) pBuffer;
			u32 nHash = m_nKernelHash;
			for (unsigned i = 0; i < nCount; i++)
			{
				nHash ^= p[i];
				nHash *= 16777619U;
			}
			m_nKernelHash = nHash;
		}

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
