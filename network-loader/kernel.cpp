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
#include "netconfig.h"
#include "rapi_chainboot.h"
#include "buildstamp.h"
#include <circle/chainboot.h>
#include <circle/logger.h>
#include <circle/sysconfig.h>
#include <assert.h>

#define HTTP_BOOT_PORT		8080

// WebDAV listens on its own port: CHTTPDaemon's wire parser only knows
// GET/HEAD/POST (see webdavserver.h), so it cannot share CHTTPBootServer's
// port 8080 -- PROPFIND/PUT/DELETE/MKCOL need a listener of their own.
#define WEBDAV_PORT		8081

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
	// Left with no address, which is how CNetSubSystem is told to use DHCP.
	// A static address from config.txt is written into the configuration
	// before Initialize() runs, which is where Circle decides between the
	// two -- see ConfigureNetwork().
	m_Net (),
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
				"pi-mame chainloader -- build time: %s", RapiBuildStamp);

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
		// Before the network, because the card carries the network
		// settings. SD access is optional in itself: without it the
		// loader still boots kernels, it just cannot serve sd/ transfers
		// -- and, having read no config.txt, it asks DHCP.
		if (   !m_EMMC.Initialize ()
		    || f_mount (&m_FileSystem, "SD:", 1) != FR_OK)
		{
			m_Logger.Write (FromKernel, LogWarning,
					"SD card not mounted, sd/ transfers disabled");
		}
	}

	if (bOK)
	{
		ConfigureNetwork ();

		bOK = m_Net.Initialize (FALSE);	// FALSE: waited for below
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
		WaitForNetwork ();
	}

	return bOK;
}

void CKernel::ConfigureNetwork (void)
{
	// The card decides. An address in config.txt's [rapi-bootloader] section
	// is used as given; no address there means DHCP. Circle chooses between
	// the two by whether the configuration holds an address when
	// Initialize() runs, so this must happen before that call.
	TRapiNetConfig Config;
	RapiReadNetConfig (&Config);

	if (!Config.bStatic)
	{
		return;			// leave it empty: CNetSubSystem uses DHCP
	}

	m_Net.GetConfig ()->SetIPAddress (Config.IPAddress);
	m_Net.GetConfig ()->SetNetMask (Config.NetMask);

	if (Config.bHaveGateway)
	{
		m_Net.GetConfig ()->SetDefaultGateway (Config.Gateway);
	}

	// The loader looks up no names of its own, so this changes nothing it
	// does. It is set because the section is one definition and whatever
	// boots next reads the same file: a payload that names its time server
	// by host name has to get the resolver from somewhere, and on a static
	// address the card is the only place there is. Circle's DHCP client
	// fills this in from the lease; nothing fills it in when the address
	// was written by hand.
	if (Config.bHaveDNSServer)
	{
		m_Net.GetConfig ()->SetDNSServer (Config.DNSServer);
	}
}

void CKernel::WaitForNetwork (void)
{
	// Static: this waits for the link. DHCP: for the lease. Neither is given
	// a deadline, because there is nothing to fall back to and nothing else
	// for this loader to be doing -- it exists to be reachable. Circle's DHCP
	// client keeps asking on its own schedule; the note below is so a reader
	// of the serial log can tell "still asking" from "wedged".
	boolean bDHCP = m_Net.GetConfig ()->IsDHCPUsed ();

	unsigned nLastReport = m_Timer.GetTicks ();
	while (!m_Net.IsRunning ())
	{
		// The net device may still be enumerating on USB.
		m_USBHCI.UpdatePlugAndPlay ();
		m_Scheduler.Yield ();

		if (m_Timer.GetTicks () - nLastReport >= 10 * HZ)
		{
			nLastReport = m_Timer.GetTicks ();
			m_Logger.Write (FromKernel, LogNotice,
					bDHCP ? "still asking DHCP for an address"
					      : "still waiting for the network link");
		}
	}

	// The whole of the network configuration, in one line: the address the
	// loader ended up with, and which of the two ways it got there.
	CString IPString;
	m_Net.GetConfig ()->GetIPAddress ()->Format (&IPString);
	m_Logger.Write (FromKernel, LogNotice, "network: %s (%s)",
			(const char *) IPString, bDHCP ? "DHCP" : "config.txt");
}

TShutdownMode CKernel::Run (void)
{
	CString IPString;
	m_Net.GetConfig ()->GetIPAddress ()->Format (&IPString);
	m_Logger.Write (FromKernel, LogNotice, "http://%s:%u/",
			(const char *) IPString, HTTP_BOOT_PORT);
	m_Logger.Write (FromKernel, LogNotice, "http://%s:%u/",
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
