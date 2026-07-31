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
#include "rapi_chainboot.h"
#include <circle/chainboot.h>
#include <circle/net/dhcpclient.h>
#include <circle/logger.h>
#include <circle/sysconfig.h>
#include <assert.h>

#define HTTP_BOOT_PORT		8080

// WebDAV listens on its own port: CHTTPDaemon's wire parser only knows
// GET/HEAD/POST (see webdavserver.h), so it cannot share CHTTPBootServer's
// port 8080 -- PROPFIND/PUT/DELETE/MKCOL need a listener of their own.
#define WEBDAV_PORT		8081

// Network configuration. The loader asks for an address by DHCP first, and
// uses these baked octets only when no server answers — so it runs both on an
// ordinary network and on a segment that hands out nothing. Edit them for your
// own LAN. The loader is a DHCP CLIENT only; it serves no addresses.
static const u8 IPAddress[]      = {192, 168, 42, 99};
static const u8 NetMask[]        = {255, 255, 255, 0};
static const u8 DefaultGateway[] = {192, 168, 42, 1};
static const u8 DNSServer[]      = {192, 168, 42, 1};

// How long a DHCP server gets to answer before the static address is used.
// One full Circle DISCOVER attempt is 4 s (its retry schedule is 4/8/16/32 s),
// so this covers the first attempt end to end: a server that is there answers
// in milliseconds, and a segment with none costs the boot only this long.
#define DHCP_WAIT_SECONDS	4

static const char FromKernel[] = "kernel";

// Remote soft reboot, set by the HTTP /reboot endpoint, honored by the
// kernel's main loop.
volatile bool g_bRebootRequested = false;

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	// Serial device 0 is the GPIO14/15 header UART on every board. Named
	// explicitly because Circle's RASPPI >= 5 default (SERIAL_DEVICE_DEFAULT
	// = 10) is the Pi 5's dedicated debug connector, not the header.
	m_Serial (0, FALSE, 0),
	m_Tee (&m_Screen, &m_Serial),		// logger fans out to glass + UART
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),	// TRUE: plug-and-play
	// Constructed with the static configuration on purpose, even though DHCP
	// is tried first: CNetSubSystem decides DHCP-vs-static by whether this
	// address is null when Initialize() runs, and its own DHCP client cannot
	// be reached or stopped afterwards. Starting static keeps that client out
	// of the picture so AcquireNetworkConfig() can own a BOUNDED one instead.
	m_Net (IPAddress, NetMask, DefaultGateway, DNSServer),
	m_EMMC (&m_Interrupt, &m_Timer, 0)
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
		// Tee every log line to both the screen and the serial UART. The
		// serial half is what a remote host captures — without it a
		// chainloader-phase crash is visible only on the glass (needing
		// an hdmi-grab to read). Do NOT autodetect via GetLogDevice():
		// with no logdev= in cmdline.txt it resolves to the screen alone,
		// which is exactly what kept the chainloader off the serial line.
		bOK = m_Logger.Initialize (&m_Tee);
	}

	if (bOK)
	{
		// Fired here, not in Run(): everything below can fail or assert
		// before Run() is ever reached, and the build identity is the
		// first thing worth knowing when reading back a crash.
		m_Logger.Write (FromKernel, LogNotice,
				"pi-mame chainloader -- compile time: " __DATE__ " " __TIME__);

		// The requested geometry (cmdline.txt width=/height=) vs what the
		// firmware actually allocated: on boards whose firmware ignores
		// the request (observed on Pi 5, which hands out the EDID-native
		// mode) these differ, and the pitch is the truth of the scanout.
		m_Logger.Write (FromKernel, LogNotice,
				"screen: requested %ux%u, buffer %ux%u pitch %u bytes",
				m_Options.GetWidth (), m_Options.GetHeight (),
				m_Screen.GetFrameBuffer ()->GetWidth (),
				m_Screen.GetFrameBuffer ()->GetHeight (),
				m_Screen.GetFrameBuffer ()->GetPitch ());
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
		// Pi 4's Ethernet is native (GENET); Pi 3/5 have no on-die NIC —
		// their only path to a network device is the USB-attached
		// controller (Pi 3: internal LAN9514; Pi 5: also USB-based on
		// some configurations), which CNetDevice::GetNetDevice() can
		// only find once USB has enumerated it. Matches menu-loader's
		// existing m_USBHCI.Initialize() (kernel.cpp) — that loader
		// needed USB for its keyboard and always had this; network-
		// loader was written Pi-4-only and never did.
		bOK = m_USBHCI.Initialize ();
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
		AcquireNetworkConfig ();
	}

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

void CKernel::AcquireNetworkConfig (void)
{
	// Ask DHCP for an address, and keep the baked static one only if nothing
	// answers within DHCP_WAIT_SECONDS.
	//
	// Circle's own DHCP path cannot do this: CNetSubSystem creates its client
	// internally, keeps it private, and its client has no bounded failure --
	// with no server it retries DISCOVER for a minute, sleeps a minute, and
	// starts again forever, logging a warning each round (which on this loader
	// would scroll the status screen a remote operator reads). Initialize()
	// would simply never return. So the client here is ours: same Circle class,
	// same protocol, but we hold the pointer, we time it, and we stop it.
	//
	// The address is dropped for the duration of the ask. Answering ARP for
	// 192.168.42.99 on a network that hands out addresses would be squatting
	// on an address this machine was never given; a DHCP client is meant to be
	// silent until it has a lease.
	m_Net.GetConfig ()->Reset ();

	CDHCPClient *pDHCPClient = new CDHCPClient (&m_Net, m_Net.GetHostname ());

	unsigned nStart = m_Timer.GetTicks ();
	while (!pDHCPClient->IsBound ())
	{
		if (m_Timer.GetTicks () - nStart >= DHCP_WAIT_SECONDS * HZ)
		{
			// Park the client rather than leave it retrying: a suspended
			// task is skipped by the scheduler for good, so it can never
			// wake up later and overwrite the static configuration below.
			pDHCPClient->Suspend ();

			m_Net.GetConfig ()->SetIPAddress (IPAddress);
			m_Net.GetConfig ()->SetNetMask (NetMask);
			m_Net.GetConfig ()->SetDefaultGateway (DefaultGateway);
			m_Net.GetConfig ()->SetDNSServer (DNSServer);

			m_Logger.Write (FromKernel, LogNotice,
					"no DHCP offer in %u seconds -- using the static address",
					DHCP_WAIT_SECONDS);

			return;
		}

		// The net device may still be enumerating on USB; keep
		// plug-and-play pumped while waiting, as Circle's own samples do.
		m_USBHCI.UpdatePlugAndPlay ();

		m_Scheduler.Yield ();
	}

	// Bound: the client has already written the leased address, mask, gateway
	// and DNS server into the live configuration.
	m_Logger.Write (FromKernel, LogNotice, "address acquired by DHCP");
}

TShutdownMode CKernel::Run (void)
{
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
			QuiesceUSB ();		// clean xHCI teardown on reboot
			m_Scheduler.Sleep (1);	// let the HTTP reply flush

			return ShutdownReboot;
		}

		// Service USB every frame: keeps xHCI enumerated and settled (so
		// the chain-boot teardown below is clean) and is the hook a future
		// keyboard/menu on this loader will drive. Must run at TASK_LEVEL,
		// which Run() is — matches the menu-loader's pump.
		m_USBHCI.UpdatePlugAndPlay ();

		m_Screen.Rotor (0, nCount);

		m_Scheduler.Yield ();
	}

	// A boot server armed chain-boot from a task callback; the xHCI may
	// have just enumerated the pushed transfer's activity. Settle it before
	// Circle tears it down, or the teardown asserts (ptrlist m_pFirst == 0).
	QuiesceUSB ();

	m_Logger.Write (FromKernel, LogNotice, "Rebooting ...");

	m_Scheduler.Sleep (1);

	// main() is now returning: from here on IsChainBootEnabled()'s caller
	// is sysinit(), running after the full ~CKernel device teardown. On the
	// Pi 5 the shared chain-boot performs its hand-off there; on the other
	// boards Circle's own path runs and this is a no-op.
	RapiChainBootMainReturning ();

	return ShutdownReboot;
}

void CKernel::QuiesceUSB (void)
{
	// Pump plug-and-play for ~2 s so any in-flight USB enumeration
	// completes and the xHCI controller is quiescent before the reboot
	// path frees its shared memory. This mirrors the menu-loader's
	// pre-auto-boot settle; without it, tearing an unsettled xHCI down
	// trips CPtrList::~CPtrList's assert (m_pFirst == 0) and panics.
	unsigned nStart = m_Timer.GetTicks ();
	while (m_Timer.GetTicks () - nStart < 2 * HZ)
	{
		m_USBHCI.UpdatePlugAndPlay ();
		m_Scheduler.Yield ();
	}
}
