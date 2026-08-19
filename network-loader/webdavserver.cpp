//
// webdavserver.cpp — network-loader's WebDAV server, see webdavserver.h
// for the protocol and why it doesn't build on CHTTPDaemon.
//
#include "webdavserver.h"
#include <circle/net/in.h>
#include <circle/net/ipaddress.h>
#include <circle/logger.h>
#include <circle/util.h>
#include <assert.h>

#define WEBDAV_SERVER_NAME	"pi-mame-chainloader-webdav/1.0"
#define WEBDAV_TIMEOUT_SECONDS	30	// bounds a stalled client, never waits forever
#define MAX_CLIENTS		4

static const char FromWebDAV[] = "webdav";

unsigned CWebDAVServer::s_nInstanceCount = 0;

static boolean IsHexDigit (char chChar)
{
	return (chChar >= '0' && chChar <= '9')
	    || (chChar >= 'a' && chChar <= 'f')
	    || (chChar >= 'A' && chChar <= 'F');
}

static unsigned HexValue (char chChar)
{
	if (chChar >= '0' && chChar <= '9')
	{
		return (unsigned) (chChar - '0');
	}

	if (chChar >= 'a' && chChar <= 'f')
	{
		return (unsigned) (chChar - 'a' + 10);
	}

	return (unsigned) (chChar - 'A' + 10);
}

// decodes "%XX" escapes in place; leaves anything else untouched
static void UrlDecode (char *pPath)
{
	char *pOut = pPath;
	for (char *pIn = pPath; *pIn != '\0'; pIn++)
	{
		if (   pIn[0] == '%'
		    && IsHexDigit (pIn[1])
		    && IsHexDigit (pIn[2]))
		{
			*pOut++ = (char) (HexValue (pIn[1]) * 16 + HexValue (pIn[2]));
			pIn += 2;
		}
		else
		{
			*pOut++ = *pIn;
		}
	}

	*pOut = '\0';
}

static const char *SkipSpaces (const char *pString)
{
	while (*pString == ' ' || *pString == '\t')
	{
		pString++;
	}

	return pString;
}

// request path "/x/y" -> FatFs "SD:/x/y"; "/" -> "SD:/"; refuses ".." (the
// DAV root is the whole card, so path traversal would escape it entirely)
static boolean MapPath (const char *pRequestPath, CString *pSdPath)
{
	if (strstr (pRequestPath, "..") != 0)
	{
		return FALSE;
	}

	if (strcmp (pRequestPath, "/") == 0)
	{
		pSdPath->Format ("SD:/");
	}
	else
	{
		pSdPath->Format ("SD:%s", pRequestPath);
	}

	return TRUE;
}

// minimal XML text escaping for names that reach an href/displayname element
static void AppendXmlEscaped (CString *pXml, const char *pText)
{
	for (const char *p = pText; *p != '\0'; p++)
	{
		switch (*p)
		{
		case '&':  pXml->Append ("&amp;");  break;
		case '<':  pXml->Append ("&lt;");   break;
		case '>':  pXml->Append ("&gt;");   break;
		case '"':  pXml->Append ("&quot;"); break;
		default:   pXml->Append (*p);       break;
		}
	}
}

CWebDAVServer::CWebDAVServer (CNetSubSystem *pNetSubSystem, u16 nPort, CSocket *pSocket)
:	m_pNetSubSystem (pNetSubSystem),
	m_pSocket (pSocket),
	m_nPort (nPort),
	m_nContentLength (0),
	m_nDepth (1),
	m_pRecvBuf (new u8[RECV_BUF_SIZE]),
	m_nRecvFill (0),
	m_nBodyStart (0),
	m_pWriteBuf (0),
	m_nWriteBufFill (0)
{
	s_nInstanceCount++;

	m_Method[0] = '\0';
	m_Path[0] = '\0';

	if (pSocket == 0)
	{
		SetName (FromWebDAV);
	}
	else
	{
		CString TaskName;
		TaskName.Format ("webdav@%lp", this);

		SetName (TaskName);
	}
}

CWebDAVServer::~CWebDAVServer (void)
{
	assert (m_pSocket == 0);

	delete [] m_pRecvBuf;
	m_pRecvBuf = 0;

	delete [] m_pWriteBuf;
	m_pWriteBuf = 0;

	m_pNetSubSystem = 0;

	s_nInstanceCount--;
}

void CWebDAVServer::Run (void)
{
	if (m_pSocket == 0)
	{
		Listener ();
	}
	else
	{
		Worker ();
	}
}

void CWebDAVServer::Listener (void)
{
	assert (m_pNetSubSystem != 0);
	m_pSocket = new CSocket (m_pNetSubSystem, IPPROTO_TCP);
	assert (m_pSocket != 0);

	if (m_pSocket->Bind (m_nPort) < 0)
	{
		CLogger::Get ()->Write (FromWebDAV, LogError, "Cannot bind socket (port %u)", m_nPort);

		delete m_pSocket;
		m_pSocket = 0;

		return;
	}

	if (m_pSocket->Listen (MAX_CLIENTS) < 0)
	{
		CLogger::Get ()->Write (FromWebDAV, LogError, "Cannot listen on socket");

		delete m_pSocket;
		m_pSocket = 0;

		return;
	}

	while (1)
	{
		CIPAddress ForeignIP;
		u16 nForeignPort;
		CSocket *pConnection = m_pSocket->Accept (&ForeignIP, &nForeignPort);
		if (pConnection == 0)
		{
			CLogger::Get ()->Write (FromWebDAV, LogWarning, "Cannot accept connection");

			continue;
		}

		if (s_nInstanceCount >= MAX_CLIENTS+1)
		{
			CLogger::Get ()->Write (FromWebDAV, LogWarning, "Too many clients");

			delete pConnection;

			continue;
		}

		new CWebDAVServer (m_pNetSubSystem, m_nPort, pConnection);
	}
}

int CWebDAVServer::ReceiveHeaders (void)
{
	m_nRecvFill = 0;

	while (m_nRecvFill < RECV_BUF_SIZE)
	{
		int nResult = m_pSocket->Receive (m_pRecvBuf + m_nRecvFill,
						   (unsigned) (RECV_BUF_SIZE - m_nRecvFill), 0);
		if (nResult <= 0)
		{
			return -1;
		}

		size_t nSearchFrom = m_nRecvFill >= 3 ? m_nRecvFill - 3 : 0;
		m_nRecvFill += (unsigned) nResult;

		for (size_t i = nSearchFrom; i + 4 <= m_nRecvFill; i++)
		{
			if (memcmp (m_pRecvBuf + i, "\r\n\r\n", 4) == 0)
			{
				m_nBodyStart = i + 4;

				return (int) i;
			}
		}
	}

	return -1;	// header block too large
}

boolean CWebDAVServer::ParseRequest (int nHeaderLen)
{
	m_nContentLength = 0;
	m_nDepth = 1;		// RFC 4918 default is "infinity"; we fold that to 1 (see header)

	m_pRecvBuf[nHeaderLen] = '\0';

	char *pSavePtr;
	char *pLine = strtok_r ((char *) m_pRecvBuf, "\r\n", &pSavePtr);
	if (pLine == 0)
	{
		return FALSE;
	}

	char *pMethodSave;
	char *pMethod = strtok_r (pLine, " ", &pMethodSave);
	char *pUri = strtok_r (0, " ", &pMethodSave);
	if (pMethod == 0 || pUri == 0)
	{
		return FALSE;
	}

	if (strlen (pMethod) >= sizeof m_Method)
	{
		return FALSE;
	}
	strcpy (m_Method, pMethod);

	char *pQuery = strchr (pUri, '?');
	if (pQuery != 0)
	{
		*pQuery = '\0';
	}

	if (pUri[0] != '/' || strlen (pUri) >= sizeof m_Path)
	{
		return FALSE;
	}
	strcpy (m_Path, pUri);
	UrlDecode (m_Path);

	while ((pLine = strtok_r (0, "\r\n", &pSavePtr)) != 0)
	{
		if (strncasecmp (pLine, "Content-Length:", 15) == 0)
		{
			m_nContentLength = (unsigned) atoi (SkipSpaces (pLine + 15));
		}
		else if (strncasecmp (pLine, "Depth:", 6) == 0)
		{
			const char *pValue = SkipSpaces (pLine + 6);
			if (strncmp (pValue, "infinity", 8) == 0)
			{
				m_nDepth = 1;	// see header comment: folded to one level
			}
			else
			{
				m_nDepth = (unsigned) atoi (pValue);
			}
		}
	}

	return TRUE;
}

void CWebDAVServer::SendSimpleResponse (int nStatus, const char *pReason,
					 const char *pContentType, const char *pBody)
{
	unsigned nBodyLength = pBody != 0 ? (unsigned) strlen (pBody) : 0;

	CString Header;
	Header.Format ("HTTP/1.1 %d %s\r\n"
		       "Server: " WEBDAV_SERVER_NAME "\r\n"
		       "Content-Type: %s\r\n"
		       "Content-Length: %u\r\n"
		       "Connection: close\r\n"
		       "\r\n", nStatus, pReason, pContentType, nBodyLength);

	// Blocking sends (flag 0), not MSG_DONTWAIT: CSocket::Send yields to the
	// scheduler only after a chunk is accepted, so a non-blocking send that
	// hits a full TCP window returns before yielding — a large body then
	// spins the caller with no yield, starving the net task that drains TCP
	// (it wedged the whole loader on a 57MB GET). Blocking gives proper
	// backpressure; the send timeout set in Worker() bounds a dead client.
	if (m_pSocket->Send ((const char *) Header, Header.GetLength (), 0) < 0)
	{
		return;
	}

	if (nBodyLength > 0)
	{
		m_pSocket->Send (pBody, nBodyLength, 0);
	}
}

void CWebDAVServer::AppendPropfindEntry (CString *pXml, const char *pHref,
					  boolean bIsCollection, unsigned long nSize)
{
	// collections get a trailing slash, unless the href already has one
	// (the DAV root itself, "/", is passed in whole)
	size_t nHrefLen = strlen (pHref);
	boolean bNeedsSlash = bIsCollection && (nHrefLen == 0 || pHref[nHrefLen-1] != '/');

	pXml->Append ("<D:response><D:href>");
	AppendXmlEscaped (pXml, pHref);
	pXml->Append (bNeedsSlash ? "/" : "");
	pXml->Append ("</D:href><D:propstat><D:prop>");

	if (bIsCollection)
	{
		pXml->Append ("<D:resourcetype><D:collection/></D:resourcetype>");
	}
	else
	{
		CString Size;
		Size.Format ("<D:resourcetype/><D:getcontentlength>%lu</D:getcontentlength>", nSize);
		pXml->Append (Size);
	}

	pXml->Append ("</D:prop><D:status>HTTP/1.1 200 OK</D:status>"
		      "</D:propstat></D:response>");
}

void CWebDAVServer::DoOptions (void)
{
	// Class-1 capability advertisement. A WebDAV client (macOS Finder,
	// davfs2, cadaver, …) sends OPTIONS before mounting and reads the DAV
	// header to decide the server speaks WebDAV; without a 200 + "DAV:"
	// here it never issues a PROPFIND. We advertise class 1 ("DAV: 1", not
	// "1,2") because there is no LOCK/UNLOCK — clients that require class-2
	// locking to mount read-write may fall back to read-only or decline.
	// SendSimpleResponse cannot carry these extra headers, so send directly.
	CString Header;
	Header.Format ("HTTP/1.1 200 OK\r\n"
		       "Server: " WEBDAV_SERVER_NAME "\r\n"
		       "DAV: 1\r\n"
		       "Allow: OPTIONS, GET, HEAD, PUT, DELETE, MKCOL, PROPFIND\r\n"
		       "MS-Author-Via: DAV\r\n"
		       "Content-Length: 0\r\n"
		       "Connection: close\r\n"
		       "\r\n");

	m_pSocket->Send ((const char *) Header, Header.GetLength (), 0);
}

void CWebDAVServer::DoPropfind (void)
{
	CString SdPath;
	if (!MapPath (m_Path, &SdPath))
	{
		SendSimpleResponse (403, "Forbidden", "text/plain", "Invalid path");
		return;
	}

	boolean bRoot = strcmp (m_Path, "/") == 0;
	boolean bIsDir = TRUE;
	FSIZE_t nSize = 0;

	if (!bRoot)
	{
		FILINFO Info;
		if (f_stat ((const char *) SdPath, &Info) != FR_OK)
		{
			SendSimpleResponse (404, "Not Found", "text/plain", "Not found");
			return;
		}

		bIsDir = (Info.fattrib & AM_DIR) != 0;
		nSize = Info.fsize;
	}

	CString Xml;
	Xml.Format ("<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
		    "<D:multistatus xmlns:D=\"DAV:\">");

	AppendPropfindEntry (&Xml, m_Path, bIsDir, (unsigned long) nSize);

	if (bIsDir && m_nDepth != 0)
	{
		DIR Dir;
		if (f_opendir (&Dir, (const char *) SdPath) == FR_OK)
		{
			FILINFO Child;
			while (   f_readdir (&Dir, &Child) == FR_OK
			       && Child.fname[0] != '\0')
			{
				CString Href;
				Href.Format ("%s%s%s", m_Path, bRoot ? "" : "/", Child.fname);

				AppendPropfindEntry (&Xml, (const char *) Href,
						     (Child.fattrib & AM_DIR) != 0,
						     (unsigned long) Child.fsize);
			}

			f_closedir (&Dir);
		}
	}

	Xml.Append ("</D:multistatus>\r\n");

	SendSimpleResponse (207, "Multi-Status", "text/xml; charset=\"utf-8\"", (const char *) Xml);
}

void CWebDAVServer::DoGet (boolean bHeadOnly)
{
	CString SdPath;
	if (!MapPath (m_Path, &SdPath))
	{
		SendSimpleResponse (403, "Forbidden", "text/plain", "Invalid path");
		return;
	}

	FILINFO Info;
	if (f_stat ((const char *) SdPath, &Info) != FR_OK)
	{
		SendSimpleResponse (404, "Not Found", "text/plain", "Not found");
		return;
	}

	if (Info.fattrib & AM_DIR)
	{
		// GET serves files; PROPFIND (Depth 0/1) is the collection listing
		SendSimpleResponse (405, "Method Not Allowed", "text/plain",
				     "GET is for files -- PROPFIND lists collections");
		return;
	}

	FIL File;
	if (f_open (&File, (const char *) SdPath, FA_READ) != FR_OK)
	{
		SendSimpleResponse (500, "Internal Server Error", "text/plain", "Cannot open file");
		return;
	}

	CString Header;
	Header.Format ("HTTP/1.1 200 OK\r\n"
		       "Server: " WEBDAV_SERVER_NAME "\r\n"
		       "Content-Type: application/octet-stream\r\n"
		       "Content-Length: %lu\r\n"
		       "Connection: close\r\n"
		       "\r\n", (unsigned long) Info.fsize);

	if (   m_pSocket->Send ((const char *) Header, Header.GetLength (), 0) >= 0
	    && !bHeadOnly)
	{
		UINT nRead;
		do
		{
			if (f_read (&File, m_pRecvBuf, (UINT) RECV_BUF_SIZE, &nRead) != FR_OK)
			{
				break;
			}

			// Blocking send: backpressure against the TCP window so a
			// large file streams at the link's pace instead of spinning
			// this loop with no yield. A negative result means the client
			// went away (or the send timed out) -- stop, don't read on.
			if (nRead > 0 && m_pSocket->Send (m_pRecvBuf, nRead, 0) < 0)
			{
				break;
			}
		}
		while (nRead == RECV_BUF_SIZE);
	}

	f_close (&File);
}

boolean CWebDAVServer::WritePutChunk (FIL *pFile, const u8 *pData, unsigned nLength)
{
	if (m_pWriteBuf == 0)
	{
		m_pWriteBuf = new u8[WRITE_BUF_SIZE];
		if (m_pWriteBuf == 0)
		{
			return FALSE;
		}
	}

	while (nLength > 0)
	{
		unsigned nSpace = (unsigned) (WRITE_BUF_SIZE - m_nWriteBufFill);
		unsigned nCopy = nLength < nSpace ? nLength : nSpace;

		memcpy (m_pWriteBuf + m_nWriteBufFill, pData, nCopy);
		m_nWriteBufFill += nCopy;
		pData += nCopy;
		nLength -= nCopy;

		if (m_nWriteBufFill == WRITE_BUF_SIZE && !FlushPutBuf (pFile))
		{
			return FALSE;
		}
	}

	return TRUE;
}

boolean CWebDAVServer::FlushPutBuf (FIL *pFile)
{
	if (m_nWriteBufFill == 0)
	{
		return TRUE;
	}

	UINT nWritten = 0;
	FRESULT Result = f_write (pFile, m_pWriteBuf, (UINT) m_nWriteBufFill, &nWritten);
	boolean bOK = Result == FR_OK && nWritten == m_nWriteBufFill;

	m_nWriteBufFill = 0;

	return bOK;
}

void CWebDAVServer::DoPut (void)
{
	CString SdPath;
	if (!MapPath (m_Path, &SdPath))
	{
		SendSimpleResponse (403, "Forbidden", "text/plain", "Invalid path");
		return;
	}

	// intermediate directories are created on demand, matching the sd/<path>
	// TFTP writes (tftpbootserver.cpp): a PUT into a fresh subtree just works
	const char *pPath = (const char *) SdPath;
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

	boolean bExisted = f_stat (pPath, 0) == FR_OK;

	FIL File;
	if (f_open (&File, pPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		SendSimpleResponse (409, "Conflict", "text/plain", "Cannot create file");
		return;
	}

	unsigned nRemaining = m_nContentLength;
	size_t nBuffered = m_nRecvFill - m_nBodyStart;
	if (nBuffered > nRemaining)
	{
		nBuffered = nRemaining;
	}

	boolean bOK = TRUE;
	if (nBuffered > 0)
	{
		bOK = WritePutChunk (&File, m_pRecvBuf + m_nBodyStart, (unsigned) nBuffered);
		nRemaining -= (unsigned) nBuffered;
	}

	while (bOK && nRemaining > 0)
	{
		unsigned nToRead = nRemaining < RECV_BUF_SIZE ? nRemaining : (unsigned) RECV_BUF_SIZE;
		int nResult = m_pSocket->Receive (m_pRecvBuf, nToRead, 0);
		if (nResult <= 0)
		{
			bOK = FALSE;
			break;
		}

		bOK = WritePutChunk (&File, m_pRecvBuf, (unsigned) nResult);
		nRemaining -= (unsigned) nResult;
	}

	if (bOK)
	{
		bOK = FlushPutBuf (&File);
	}

	f_close (&File);

	if (!bOK)
	{
		SendSimpleResponse (500, "Internal Server Error", "text/plain", "Write failed");
	}
	else if (bExisted)
	{
		SendSimpleResponse (204, "No Content", "text/plain", 0);
	}
	else
	{
		SendSimpleResponse (201, "Created", "text/plain", 0);
	}
}

void CWebDAVServer::DoDelete (void)
{
	CString SdPath;
	if (!MapPath (m_Path, &SdPath) || strcmp (m_Path, "/") == 0)
	{
		SendSimpleResponse (403, "Forbidden", "text/plain", "Cannot delete the DAV root");
		return;
	}

	FRESULT Result = f_unlink ((const char *) SdPath);
	switch (Result)
	{
	case FR_OK:
		SendSimpleResponse (204, "No Content", "text/plain", 0);
		break;

	case FR_NO_FILE:
	case FR_NO_PATH:
		SendSimpleResponse (404, "Not Found", "text/plain", "Not found");
		break;

	case FR_DENIED:
		SendSimpleResponse (409, "Conflict", "text/plain", "Directory not empty");
		break;

	default:
		SendSimpleResponse (500, "Internal Server Error", "text/plain", "Delete failed");
		break;
	}
}

void CWebDAVServer::DoMkcol (void)
{
	CString SdPath;
	if (!MapPath (m_Path, &SdPath))
	{
		SendSimpleResponse (403, "Forbidden", "text/plain", "Invalid path");
		return;
	}

	FRESULT Result = f_mkdir ((const char *) SdPath);
	switch (Result)
	{
	case FR_OK:
		SendSimpleResponse (201, "Created", "text/plain", 0);
		break;

	case FR_EXIST:
		SendSimpleResponse (405, "Method Not Allowed", "text/plain", "Already exists");
		break;

	case FR_NO_PATH:
		SendSimpleResponse (409, "Conflict", "text/plain", "Parent does not exist");
		break;

	default:
		SendSimpleResponse (500, "Internal Server Error", "text/plain", "MKCOL failed");
		break;
	}
}

void CWebDAVServer::Worker (void)
{
	assert (m_pSocket != 0);

	m_pSocket->SetOptionReceiveTimeout (WEBDAV_TIMEOUT_SECONDS * 1000000);
	m_pSocket->SetOptionSendTimeout (WEBDAV_TIMEOUT_SECONDS * 1000000);

	int nHeaderLen = ReceiveHeaders ();
	if (nHeaderLen >= 0 && ParseRequest (nHeaderLen))
	{
		if (strcmp (m_Method, "OPTIONS") == 0)
		{
			DoOptions ();
		}
		else if (strcmp (m_Method, "GET") == 0)
		{
			DoGet (FALSE);
		}
		else if (strcmp (m_Method, "HEAD") == 0)
		{
			DoGet (TRUE);
		}
		else if (strcmp (m_Method, "PUT") == 0)
		{
			DoPut ();
		}
		else if (strcmp (m_Method, "DELETE") == 0)
		{
			DoDelete ();
		}
		else if (strcmp (m_Method, "MKCOL") == 0)
		{
			DoMkcol ();
		}
		else if (strcmp (m_Method, "PROPFIND") == 0)
		{
			DoPropfind ();
		}
		else
		{
			SendSimpleResponse (501, "Method Not Implemented", "text/plain",
					     "Unsupported method");
		}

		CLogger::Get ()->Write (FromWebDAV, LogDebug, "%s %s", m_Method, m_Path);
	}
	else if (nHeaderLen >= 0)
	{
		SendSimpleResponse (400, "Bad Request", "text/plain", "Malformed request");
	}
	// nHeaderLen < 0: connection closed or oversize headers -- nothing to reply to

	delete m_pSocket;
	m_pSocket = 0;
}
