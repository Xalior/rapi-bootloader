//
// tftpbootserver.h
//
// pi-mame chainloader TFTP server, extended from Circle sample 38:
//   put inject        -> stashes a defaults-string for the NEXT kernel push
//   put kernel*.img   -> staged in RAM (high heap), 0x800-patched, chain-booted
//   put sd/<path>     -> written to the SD card's FAT filesystem
//   get sd/<path>     -> read back from the SD card
//
// The "inject" push is dev-tooling only (a defaults-string push path): its bytes
// become the argv defaults-string stamped into the next kernel image's 0x800
// block via the shared PatchDefaults writer, so any machine can boot with any
// parameters over the wire without a rebuild. No inject push -> no patch ->
// today's behaviour exactly. Never product surface.
//
// Based on Circle sample/38-bootloader (GPLv3), R. Stange.
//
#ifndef _tftpbootserver_h
#define _tftpbootserver_h

#include <circle/net/tftpdaemon.h>
#include <circle/net/netsubsystem.h>
#include <circle/types.h>
#include <fatfs/ff.h>
#include "defaultsblock.h"

class CTFTPBootServer : public CTFTPDaemon
{
public:
	CTFTPBootServer (CNetSubSystem *pNetSubSystem, size_t nMaxKernelSize);
	~CTFTPBootServer (void);

	boolean FileOpen (const char *pFileName);	// TFTP GET
	boolean FileCreate (const char *pFileName);	// TFTP PUT
	boolean FileClose (void);
	int FileRead (void *pBuffer, unsigned nCount);
	int FileWrite (const void *pBuffer, unsigned nCount);

private:
	enum TMode
	{
		ModeNone,
		ModeKernel,	// receiving a chain-boot payload into RAM
		ModeInject,	// receiving a defaults-string for the next payload
		ModeSDWrite,	// writing a file to the SD card
		ModeSDRead	// reading a file from the SD card
	};

	size_t m_nMaxKernelSize;

	TMode m_Mode;
	u8 *m_pKernelBuffer;
	unsigned m_nCurrentOffset;
	FIL m_File;

	// The pending defaults-string injection (dev tooling): an "inject" push
	// fills this and arms m_bInjectPending; the very next kernel push patches
	// it into the image's 0x800 block and disarms (one-shot). The daemon runs
	// one persistent instance handling transfers sequentially, so this state
	// legitimately carries from the inject push to the following kernel push.
	boolean m_bInjectPending;
	size_t m_nInjectFill;
	char m_InjectText[DEFAULTS_BUFFER_BYTES];

	// SD writes are batched: FatFs turns per-TFTP-block (512 byte) writes
	// into synchronous card transactions, which caps large transfers far
	// below the card's sequential write rate.
	static const size_t WRITE_BUF_SIZE = 1024 * 1024;
	u8 *m_pWriteBuf;
	size_t m_nWriteBufFill;

	boolean FlushWriteBuf (void);
};

#endif
