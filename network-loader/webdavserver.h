//
// webdavserver.h — minimal WebDAV surface onto the chainloader's SD card.
//
// Circle's CHTTPDaemon (circle/include/circle/net/httpdaemon.h) only ever
// parses GET/HEAD/POST: THTTPRequestMethod has no other members and
// ParseMethod() (circle/lib/net/httpdaemon.cpp) rejects anything else with
// HTTPMethodNotImplemented before a subclass's GetContent() is ever called —
// both are private, non-virtual. PROPFIND/PUT/DELETE/MKCOL can therefore
// never reach a CHTTPDaemon subclass, however it's overridden. This class
// speaks HTTP/WebDAV directly over CSocket instead — the same TCP primitive
// CHTTPDaemon itself is built on — and listens on its own port (see
// kernel.cpp) because CHTTPBootServer already owns 8080 with a daemon whose
// wire parser cannot be widened from outside Circle.
//
// Verbs: PROPFIND (Depth 0/1 directory listing), GET, HEAD, PUT, DELETE,
// MKCOL. The whole SD card is the DAV root: request path "/x/y" maps to
// FatFs path "SD:/x/y". One TCP connection serves exactly one request
// (Connection: close), matching CHTTPDaemon's own model.
//
#ifndef _webdavserver_h
#define _webdavserver_h

#include <circle/sched/task.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/socket.h>
#include <circle/string.h>
#include <circle/types.h>
#include <fatfs/ff.h>

class CWebDAVServer : public CTask
{
public:
	CWebDAVServer (CNetSubSystem *pNetSubSystem, u16 nPort,
		       CSocket *pSocket = 0);	// is 0 for 1st created instance (listener)
	~CWebDAVServer (void);

	void Run (void);

private:
	void Listener (void);		// accepts incoming connections and creates worker tasks
	void Worker (void);		// serves the single request on an accepted connection

	// fills m_pRecvBuf up to the blank line ending the request headers;
	// returns the header block length, or < 0 on error/oversize/closed
	int ReceiveHeaders (void);

	// splits the header block (m_pRecvBuf[0..nHeaderLen)) into method,
	// path (percent-decoded) and the headers we care about
	boolean ParseRequest (int nHeaderLen);

	void SendSimpleResponse (int nStatus, const char *pReason,
				  const char *pContentType, const char *pBody);

	void DoPropfind (void);
	void DoGet (boolean bHeadOnly);
	void DoPut (void);
	void DoDelete (void);
	void DoMkcol (void);

	void AppendPropfindEntry (CString *pXml, const char *pHref,
				   boolean bIsCollection, unsigned long nSize);

	// batches PUT body writes so FatFs sees megabyte-sized transactions
	// instead of one per TCP segment (same reasoning as the TFTP sd/
	// write path in tftpbootserver.cpp)
	boolean WritePutChunk (FIL *pFile, const u8 *pData, unsigned nLength);
	boolean FlushPutBuf (FIL *pFile);

private:
	CNetSubSystem *m_pNetSubSystem;
	CSocket	      *m_pSocket;
	u16	       m_nPort;

	// parsed request
	char m_Method[16];
	char m_Path[512];		// "/"-rooted, percent-decoded, ".." refused
	unsigned m_nContentLength;	// PUT body length, from Content-Length
	unsigned m_nDepth;		// PROPFIND Depth: 0 or 1 ("infinity" folds to 1)

	static const size_t RECV_BUF_SIZE = 4096;
	u8    *m_pRecvBuf;	// header scratch, then reused for GET/PUT body chunks
	size_t m_nRecvFill;	// bytes valid in m_pRecvBuf after ReceiveHeaders()
	size_t m_nBodyStart;	// offset of the first body byte already in m_pRecvBuf

	static const size_t WRITE_BUF_SIZE = 1024 * 1024;
	u8    *m_pWriteBuf;	// allocated on first PUT only
	size_t m_nWriteBufFill;

	static unsigned s_nInstanceCount;	// bounds concurrent connections, like CHTTPDaemon
};

#endif
