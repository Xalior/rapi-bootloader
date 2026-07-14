//
// kernel.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2015-2019  R. Stange <rsta2@o2online.de>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include "kernel.h"
#include "httpbootserver.h"
#include "tftpbootserver.h"
#include "webdavserver.h"
#include "eeeprobe.h"
#include <circle/chainboot.h>
#include <circle/logger.h>
#include <circle/sysconfig.h>
#include <assert.h>

#define HTTP_BOOT_PORT		8080

// WebDAV listens on its own port: CHTTPDaemon's wire parser only knows
// GET/HEAD/POST (see webdavserver.h), so it cannot share CHTTPBootServer's
// port 8080 -- PROPFIND/PUT/DELETE/MKCOL need a listener of their own.
#define WEBDAV_PORT		8081

// Network configuration — static, for a segment with no DHCP server.
// The loader claims 192.168.42.99; edit these octets for your own LAN.
//#define USE_DHCP

#ifndef USE_DHCP
static const u8 IPAddress[]      = {192, 168, 42, 99};
static const u8 NetMask[]        = {255, 255, 255, 0};
static const u8 DefaultGateway[] = {192, 168, 42, 1};
static const u8 DNSServer[]      = {192, 168, 42, 1};
#endif

static const char FromKernel[] = "kernel";

// Remote soft reboot, set by the HTTP /reboot endpoint, honored by the
// kernel's main loop.
volatile bool g_bRebootRequested = false;

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer)
#ifndef USE_DHCP
	, m_Net (IPAddress, NetMask, DefaultGateway, DNSServer)
#endif
	, m_EMMC (&m_Interrupt, &m_Timer, 0)
{
}

CKernel::~CKernel (void)
{
}

boolean CKernel::Initialize (void)
{
	boolean bOK = TRUE;

	if (bOK)
	{
		bOK = m_Screen.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Serial.Initialize (115200);
	}

	if (bOK)
	{
		CDevice *pTarget = m_DeviceNameService.GetDevice (m_Options.GetLogDevice (), FALSE);
		if (pTarget == 0)
		{
			pTarget = &m_Screen;
		}

		bOK = m_Logger.Initialize (pTarget);
	}

	if (bOK)
	{
		bOK = m_Interrupt.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Timer.Initialize ();
	}

	if (bOK)
	{
		bOK = m_Net.Initialize ();
	}

#if RASPPI == 4
	if (bOK)
	{
		// The BCM54213 PHY powers up advertising Energy-Efficient
		// Ethernet and the link partner accepts; Circle's genet driver
		// manages EEE on neither the PHY nor the MAC, so LPI silently
		// eats locally-originated UDP frames on the boots where the
		// partners actually use it. Clear the advertisement and
		// renegotiate: EEE never established, egress always healthy.
		// GENET/BCM54213 is Pi 4 only (see eeeprobe.cpp) — no-op on
		// Pi 3/5, which don't have this MAC/PHY.
		TEEEState State;
		EEEProbeRead (&State);
		int nAdvAfter = EEEProbeDisable ();
		CLogger::Get ()->Write (FromKernel, LogNotice,
					"EEE disabled (adv %04x -> %04x, partner %04x)",
					State.nEEEAdv, nAdvAfter, State.nEEELpAbility);

		// Renegotiation drops the link for a moment; give it time to
		// come back before the boot servers matter.
		m_Scheduler.Sleep (3);
	}
#endif

	if (bOK)
	{
		// SD write access is optional: without it the chainloader still
		// boots kernels, it just can't service sd/ file transfers.
		if (   !m_EMMC.Initialize ()
		    || f_mount (&m_FileSystem, "SD:", 1) != FR_OK)
		{
			m_Logger.Write ("kernel", LogWarning,
					"SD card not mounted, sd/ transfers disabled");
		}
	}

	return bOK;
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice, "pi-mame chainloader -- compile time: " __DATE__ " " __TIME__);

	CString IPString;
	m_Net.GetConfig ()->GetIPAddress ()->Format (&IPString);
	m_Logger.Write (FromKernel, LogNotice, "Open \"http://%s:%u/\" in your web browser!",
			(const char *) IPString, HTTP_BOOT_PORT);
	m_Logger.Write (FromKernel, LogNotice,
			"Try \"tftp -m binary %s -c put kernel.img\" from another computer!",
			(const char *) IPString);
	m_Logger.Write (FromKernel, LogNotice,
			"WebDAV (PROPFIND/GET/PUT/DELETE/MKCOL) at \"http://%s:%u/\"",
			(const char *) IPString, WEBDAV_PORT);

	new CHTTPBootServer (&m_Net, HTTP_BOOT_PORT, KERNEL_MAX_SIZE + 2000);
	new CTFTPBootServer (&m_Net, KERNEL_MAX_SIZE);
	new CWebDAVServer (&m_Net, WEBDAV_PORT);

	extern volatile bool g_bRebootRequested;
	for (unsigned nCount = 0; !IsChainBootEnabled (); nCount++)
	{
		if (g_bRebootRequested)
		{
			m_Logger.Write (FromKernel, LogNotice, "Reboot requested");
			m_Scheduler.Sleep (1);	// let the HTTP reply flush

			return ShutdownReboot;
		}

		m_Screen.Rotor (0, nCount);

		m_Scheduler.Yield ();
	}

	m_Logger.Write (FromKernel, LogNotice, "Rebooting ...");

	m_Scheduler.Sleep (1);

	return ShutdownReboot;
}
