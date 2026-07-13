//
// kernel.h
//
// pi-mame boot picker — the platform card's front door. A dead-simple
// boot-world (single-core Circle) app: it prints the text list from
// bootmenu.cfg, takes a keyboard pick, patches the chosen defaults-string
// into a staged platform kernel image at offset 0x800, and chain-boots it.
//
// EASY mode: entries in file order, the top line is where the cursor first
// rests, no timeout, nothing boots until a human picks.
//
#ifndef _kernel_h
#define _kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/types.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include "bootmenu.h"

// The card's config and the MAME core image. One core binary per card; the
// machine name lives in the pick. The core is named per board —
// pi-mame-core-<SYSTEMBIT>.img — so one card can carry a payload per board
// (the PoC3 multi-board matrix), mirroring the firmware's kernel8*.img
// convention. The picker itself ships alongside it as
// pi-mame-boot-<SYSTEMBIT>.img. PoC2 is Pi 4 only; PoC3 resolves SYSTEMBIT at
// runtime (Circle CMachineInfo::GetMachineModel) to add rpi3 / rpi5.
#define SYSTEMBIT		"rpi4"
#define BOOTMENU_CFG_PATH	"SD:/bootmenu.cfg"
#define PLATFORM_KERNEL_PATH	"SD:/pi-mame-core-" SYSTEMBIT ".img"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

private:
	// Draw the menu (clears the screen). Highlights the cursor line and shows
	// only a window of entries when the list is longer than one screen page,
	// scrolling that window to keep the cursor visible.
	void DrawMenu (void);
	void WriteString (const char *pString);

	// Entries that fit in one page: the screen's text rows less the header and
	// footer. Sizes the scrolling window; always at least one.
	unsigned VisibleRows (void) const;

	// Load PLATFORM_KERNEL_PATH into the staging buffer, patch the chosen
	// defaults-string at 0x800, and arm chain-boot. A kernel that cannot be
	// booted (missing image, bad read, magic/patch refusal) is fatal — see
	// Fatal() — never a silent return to the menu.
	void BootSelection (unsigned nIndex);

	// A kernel we cannot boot is fatal: show the reason on the glass (which
	// the menu redraw can never wipe) and hang. Every menu entry boots the
	// same platform image, so a malformed one fails identically each time —
	// there is nothing to fall back to.
	void Fatal (const char *pMessage) __attribute__ ((noreturn));

	// USB keyboard handling. The raw handler runs in interrupt context and
	// only records edge-triggered intents; the main loop acts on them.
	static void KeyStatusHandlerRaw (unsigned char ucModifiers,
					 const unsigned char RawKeys[6]);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;
	CSerialDevice		m_Serial;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CScheduler		m_Scheduler;
	CUSBHCIDevice		m_USBHCI;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;

	CUSBKeyboardDevice * volatile m_pKeyboard;

	CBootMenu		m_Menu;
	u8 *			m_pImageBuffer;		// staging, lazily allocated

	unsigned		m_nCursor;		// highlighted entry
	unsigned		m_nTopVisible;		// first entry drawn (window top)

	// Edge-triggered keyboard intents, produced in interrupt context and
	// drained by the main loop.
	volatile int		m_nMoveAccum;		// +down / -up, accumulated
	volatile int		m_nJumpTo;		// digit jump, -1 = none
	volatile int		m_nPageAccum;		// +pgdn / -pgup, accumulated
	volatile boolean	m_bSelectPending;	// Enter pressed

	// Previous raw report, for newly-pressed edge detection.
	unsigned char		m_PrevRawKeys[6];

	static CKernel *s_pThis;
};

#endif
