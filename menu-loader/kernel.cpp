//
// kernel.cpp
//
// pi-mame boot picker. See kernel.h.
//
#include "kernel.h"
#include "defaultsblock.h"
#include <circle/chainboot.h>
#include <circle/logger.h>
#include <circle/string.h>
#include <circle/util.h>
#include <circle/new.h>
#include <circle/memory.h>

static const char FromKernel[] = "picker";

// USB HID usage codes we care about (raw mode). Names are prefixed to avoid
// Circle's TSpecialKey enum (KeyUp/KeyDown/... in circle/input/keymap.h).
static const unsigned char HidKeyUp	= 0x52;
static const unsigned char HidKeyDown	= 0x51;
static const unsigned char HidKeyEnter	= 0x28;
static const unsigned char HidKeyKpEnter = 0x58;
// Digits 1..9 are contiguous usage codes 0x1E..0x26.
static const unsigned char HidKeyDigit1	= 0x1E;
static const unsigned char HidKeyDigit9	= 0x26;
static const unsigned char HidKeyPageUp	= 0x4B;
static const unsigned char HidKeyPageDown = 0x4E;

CKernel *CKernel::s_pThis = 0;

CKernel::CKernel (void)
:	m_Screen (m_Options.GetWidth (), m_Options.GetHeight ()),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),	// TRUE: plug-and-play
	m_EMMC (&m_Interrupt, &m_Timer, 0),
	m_pKeyboard (0),
	m_pImageBuffer (0),
	m_nCursor (0),
	m_nTopVisible (0),
	m_nMoveAccum (0),
	m_nJumpTo (-1),
	m_nPageAccum (0),
	m_bSelectPending (FALSE)
{
	s_pThis = this;
	memset (m_PrevRawKeys, 0, sizeof m_PrevRawKeys);
	m_ActLED.Blink (5);	// show we are alive
}

CKernel::~CKernel (void)
{
	s_pThis = 0;
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
		// Logger goes straight to the serial UART, exactly as the MAME
		// payload does it (kernel.cpp: m_Logger.Initialize (&m_Serial)):
		// a serial console captures every diagnostic on serial, and the
		// HDMI screen stays exclusively the menu's — logger lines never
		// corrupt the drawn list. Do NOT autodetect via GetLogDevice():
		// with no logdev= in cmdline.txt it defaults to the screen (tty1),
		// which is exactly what put the log on the glass and off the serial.
		bOK = m_Logger.Initialize (&m_Serial);
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
		bOK = m_USBHCI.Initialize ();
	}

	if (bOK)
	{
		if (   !m_EMMC.Initialize ()
		    || f_mount (&m_FileSystem, "SD:", 1) != FR_OK)
		{
			m_Logger.Write (FromKernel, LogError,
					"SD card not mounted — no menu to show");
			bOK = FALSE;
		}
	}

	return bOK;
}

void CKernel::WriteString (const char *pString)
{
	m_Screen.Write (pString, strlen (pString));
}

void CKernel::Fatal (const char *pMessage)
{
	// Doom on the glass, then stop. Serial gets it too, but the
	// screen is the user-facing surface for a card that cannot boot — and
	// unlike a per-step log line, a full-screen halt can never be wiped by a
	// menu redraw.
	m_Logger.Write (FromKernel, LogError, "FATAL: %s — halted", pMessage);

	CString Msg;
	Msg.Format ("\x1b[H\x1b[J"			// clear the menu away
		    "*** BOOT FAILED ***\n\n"
		    "  %s\n\n"
		    "  Cannot boot %s.\n"
		    "  Fix the card and power-cycle. Halted.\n",
		    pMessage, PLATFORM_KERNEL_PATH);
	WriteString (Msg);

	for (;;)
	{
		m_Scheduler.Yield ();
	}
}

unsigned CKernel::VisibleRows (void) const
{
	// The screen's text rows, less the 4-line header and a 2-line footer
	// (blank + status). Computed from the real grid so it adapts to whatever
	// geometry cmdline.txt gave this card. Always leave room for one entry.
	unsigned nRows = m_Screen.GetRows ();
	return nRows > 6 ? nRows - 6 : 1;
}

void CKernel::DrawMenu (void)
{
	const unsigned nCount = m_Menu.GetCount ();
	const unsigned nPage  = VisibleRows ();

	// Scroll the window the least amount that keeps the cursor visible: the
	// list feels anchored, only its edges move. A long menu (the Commodore
	// card is 29 machines) shows one screen-page at a time instead of spilling
	// off the bottom.
	if (m_nCursor < m_nTopVisible)
	{
		m_nTopVisible = m_nCursor;
	}
	else if (m_nCursor >= m_nTopVisible + nPage)
	{
		m_nTopVisible = m_nCursor - nPage + 1;
	}
	// Never scroll past the end into blank rows: the last page is full and
	// top-aligned to the final nPage entries.
	if (nCount > nPage && m_nTopVisible > nCount - nPage)
	{
		m_nTopVisible = nCount - nPage;
	}
	if (nCount <= nPage)
	{
		m_nTopVisible = 0;
	}

	unsigned nEnd = m_nTopVisible + nPage;
	if (nEnd > nCount)
	{
		nEnd = nCount;
	}

	// Home the cursor and clear to end of display, then redraw.
	WriteString ("\x1b[H\x1b[J");
	WriteString ("pi-mame boot picker\n");
	WriteString ("select a machine, then press Enter\n");
	WriteString ("(up/down move, PgUp/PgDn page, 1-9 jump)\n\n");

	for (unsigned i = m_nTopVisible; i < nEnd; i++)
	{
		CString Line;
		if (i == m_nCursor)
		{
			// Reverse video for the resting line.
			Line.Format ("\x1b[7m> %u. %s\x1b[0m\n",
				     i + 1, m_Menu.GetLabel (i));
		}
		else
		{
			Line.Format ("  %u. %s\n", i + 1, m_Menu.GetLabel (i));
		}
		WriteString (Line);
	}

	// Footer only when the list needs paging: a short menu keeps its clean,
	// indicator-free look. No trailing newline — writing one on the bottom
	// row would scroll the whole page and corrupt it.
	if (nCount > nPage)
	{
		CString Foot;
		Foot.Format ("\n  [ %u-%u of %u ]   %s%s",
			     m_nTopVisible + 1, nEnd, nCount,
			     m_nTopVisible > 0 ? "^PgUp " : "",
			     nEnd < nCount ? "vPgDn" : "");
		WriteString (Foot);
	}
}

void CKernel::BootSelection (unsigned nIndex)
{
	const char *pLabel  = m_Menu.GetLabel (nIndex);
	const char *pString = m_Menu.GetString (nIndex);
	if (pLabel == 0 || pString == 0)
	{
		Fatal ("selected menu entry has no label or defaults string");
	}

	CString Msg;
	Msg.Format ("\nLoading %s (%s) ...\n", PLATFORM_KERNEL_PATH, pLabel);
	WriteString (Msg);

	// The load path logs to serial at every step with the real codes and
	// numbers, so the serial log tells us exactly where and why a boot dies (the
	// on-screen messages get wiped by the menu redraw).
	m_Logger.Write (FromKernel, LogNotice,
			"BootSelection: index %u, label '%s', string '%s'",
			nIndex, pLabel, pString);

	// Stage the single platform kernel image in the high heap (>1 GB) so a
	// large image can never overlap its own copy destination at 0x80000.
	if (m_pImageBuffer == 0)
	{
		m_Logger.Write (FromKernel, LogNotice,
				"Staging alloc: new (HEAP_HIGH) u8[KERNEL_MAX_SIZE], "
				"KERNEL_MAX_SIZE = %llu bytes",
				(unsigned long long) KERNEL_MAX_SIZE);

		m_pImageBuffer = new (HEAP_HIGH) u8[KERNEL_MAX_SIZE];
		if (m_pImageBuffer == 0)
		{
			m_Logger.Write (FromKernel, LogError,
					"Staging alloc FAILED (HEAP_HIGH returned null) "
					"for %llu bytes",
					(unsigned long long) KERNEL_MAX_SIZE);
			Fatal ("out of memory staging the platform image");
		}

		m_Logger.Write (FromKernel, LogNotice,
				"Staging alloc OK: buffer at %p", m_pImageBuffer);
	}
	else
	{
		m_Logger.Write (FromKernel, LogNotice,
				"Staging buffer reused at %p", m_pImageBuffer);
	}

	FIL File;
	FRESULT OpenResult = f_open (&File, PLATFORM_KERNEL_PATH, FA_READ);
	if (OpenResult != FR_OK)
	{
		m_Logger.Write (FromKernel, LogError,
				"f_open(%s) FAILED, FR_ code = %u",
				PLATFORM_KERNEL_PATH, (unsigned) OpenResult);
		Msg.Format ("cannot open %s (FR=%u)",
			    PLATFORM_KERNEL_PATH, (unsigned) OpenResult);
		Fatal ((const char *) Msg);
	}
	m_Logger.Write (FromKernel, LogNotice, "f_open OK (FR=0)");

	FSIZE_t nSize = f_size (&File);
	m_Logger.Write (FromKernel, LogNotice,
			"f_size = %llu bytes (KERNEL_MAX_SIZE = %llu)",
			(unsigned long long) nSize,
			(unsigned long long) KERNEL_MAX_SIZE);
	if (nSize == 0 || nSize > KERNEL_MAX_SIZE)
	{
		m_Logger.Write (FromKernel, LogError,
				"Size check FAILED: %llu out of range (0, %llu]",
				(unsigned long long) nSize,
				(unsigned long long) KERNEL_MAX_SIZE);
		f_close (&File);
		Fatal ("platform image size out of range");
	}

	// Read the whole image (FatFs reads are capped at UINT per call).
	m_Logger.Write (FromKernel, LogNotice, "Reading image ...");
	size_t nOffset = 0;
	boolean bReadOK = TRUE;
	while (nOffset < (size_t) nSize)
	{
		size_t nChunk = (size_t) nSize - nOffset;
		if (nChunk > 0x1000000)		// 16 MB per read
		{
			nChunk = 0x1000000;
		}
		UINT nRead = 0;
		FRESULT ReadResult = f_read (&File, m_pImageBuffer + nOffset,
					     (UINT) nChunk, &nRead);
		if (ReadResult != FR_OK || nRead == 0)
		{
			m_Logger.Write (FromKernel, LogError,
					"f_read FAILED at offset %llu: FR_ code = %u, "
					"nRead = %u (requested %llu)",
					(unsigned long long) nOffset,
					(unsigned) ReadResult, (unsigned) nRead,
					(unsigned long long) nChunk);
			bReadOK = FALSE;
			break;
		}
		nOffset += nRead;
	}
	f_close (&File);

	if (!bReadOK || nOffset != (size_t) nSize)
	{
		m_Logger.Write (FromKernel, LogError,
				"Read incomplete: %llu of %llu bytes",
				(unsigned long long) nOffset,
				(unsigned long long) nSize);
		Fatal ("read error on the platform image");
	}
	m_Logger.Write (FromKernel, LogNotice,
			"Read OK: %llu bytes staged", (unsigned long long) nOffset);

	// Patch the chosen defaults-string into the staged image at 0x800. The
	// patcher verifies the magic seatbelt and enforces the block capacity;
	// any refusal leaves the menu up and boots nothing.
	TPatchResult Result = PatchDefaults (m_pImageBuffer, (size_t) nSize, pString);
	if (Result != PatchOK)
	{
		m_Logger.Write (FromKernel, LogError,
				"PatchDefaults refused (TPatchResult %u): %s",
				(unsigned) Result, PatchResultString (Result));
		Msg.Format ("malformed image - patch refused: %s",
			    PatchResultString (Result));
		Fatal ((const char *) Msg);
	}

	m_Logger.Write (FromKernel, LogNotice,
			"PatchDefaults OK; booting %s with defaults: %s",
			pLabel, pString);
	WriteString ("  patched, chain-booting ...\n");

	// Arm the chain-boot; the main loop notices IsChainBootEnabled() and
	// returns, and Circle's reboot path hands control to the staged image.
	EnableChainBoot (m_pImageBuffer, (size_t) nSize);
	m_Logger.Write (FromKernel, LogNotice,
			"EnableChainBoot armed (%llu bytes from %p)",
			(unsigned long long) nSize, m_pImageBuffer);
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write (FromKernel, LogNotice,
			"pi-mame boot picker — compile time: " __DATE__ " " __TIME__);

	m_Menu.Load (BOOTMENU_CFG_PATH);

	if (m_Menu.GetCount () == 0)
	{
		// EASY mode: with no entries there is nothing a human can pick, so
		// nothing boots. Say so and idle — never auto-boot anything.
		WriteString ("\x1b[H\x1b[J");
		WriteString ("pi-mame boot picker\n\n");
		WriteString ("no usable entries in " BOOTMENU_CFG_PATH "\n");
		WriteString ("(one 'label|defaults-string' per line)\n");
		m_Logger.Write (FromKernel, LogError,
				"no usable entries in %s — nothing to boot",
				BOOTMENU_CFG_PATH);
		for (;;)
		{
			m_Scheduler.Yield ();
		}
	}

	if (m_Menu.GetCount () == 1)
	{
		// One entry: nothing to choose, so boot it directly — no menu, no
		// keypress. A single-line bootmenu.cfg thus drives the whole
		// picker → patch → chain-boot path unattended (dev self-iteration).
		// First let the USB stack finish enumerating: the menu path pumps
		// UpdatePlugAndPlay every frame while a human decides, so xHCI is
		// settled by the time we chain-boot. Auto-boot has no such pause, and
		// tearing an unsettled xHCI down asserts (ptrlist m_pFirst == 0). Pump
		// it for ~2 s here so the teardown is clean.
		unsigned nStart = m_Timer.GetTicks ();
		while (m_Timer.GetTicks () - nStart < 2 * HZ)
		{
			m_USBHCI.UpdatePlugAndPlay ();
			m_Scheduler.Yield ();
		}
		m_Logger.Write (FromKernel, LogNotice,
				"single entry — auto-booting '%s'",
				m_Menu.GetLabel (0));
		BootSelection (0);
	}
	else
	{
		DrawMenu ();
	}

	while (!IsChainBootEnabled ())
	{
		// Attach the keyboard once it appears; poll plug-and-play here at
		// TASK_LEVEL as Circle requires.
		boolean bUpdated = m_USBHCI.UpdatePlugAndPlay ();
		if (bUpdated && m_pKeyboard == 0)
		{
			m_pKeyboard = (CUSBKeyboardDevice *)
				m_DeviceNameService.GetDevice ("ukbd1", FALSE);
			if (m_pKeyboard != 0)
			{
				m_pKeyboard->RegisterRemovedHandler (
					KeyboardRemovedHandler);
				m_pKeyboard->RegisterKeyStatusHandlerRaw (
					KeyStatusHandlerRaw);
			}
		}

		if (m_pKeyboard != 0)
		{
			// Must not run in interrupt context; does nothing in raw mode
			// but keeps parity with cooked-mode LED handling.
			m_pKeyboard->UpdateLEDs ();
		}

		// Debug UART key injection. The boot world is always serial-enabled
		// (no appliance surface, no performance concern), so bytes arriving on
		// the serial RX set the very same intents the USB keyboard does — a
		// serial console can script the menu. One ASCII byte per action:
		//   j down   k up   f page-down   b page-up   Enter select   1-9 jump
		char InjBuf[32];
		int nInj = m_Serial.Read (InjBuf, sizeof InjBuf);
		for (int i = 0; i < nInj; i++)
		{
			char c = InjBuf[i];
			if	(c == 'j')		m_nMoveAccum++;
			else if (c == 'k')		m_nMoveAccum--;
			else if (c == 'f')		m_nPageAccum++;
			else if (c == 'b')		m_nPageAccum--;
			else if (c == '\r' || c == '\n')m_bSelectPending = TRUE;
			else if (c >= '1' && c <= '9')	m_nJumpTo = c - '1';
		}

		// Drain keyboard intents recorded by the interrupt handler.
		boolean bRedraw = FALSE;

		int nMove = m_nMoveAccum;
		if (nMove != 0)
		{
			m_nMoveAccum = 0;
			int nPos = (int) m_nCursor + nMove;
			if (nPos < 0)
			{
				nPos = 0;
			}
			if (nPos >= (int) m_Menu.GetCount ())
			{
				nPos = (int) m_Menu.GetCount () - 1;
			}
			if ((unsigned) nPos != m_nCursor)
			{
				m_nCursor = (unsigned) nPos;
				bRedraw = TRUE;
			}
		}

		int nPage = m_nPageAccum;
		if (nPage != 0)
		{
			m_nPageAccum = 0;
			// PgUp/PgDn flip the window a whole page at a time and land the
			// cursor at the top of the new page — true paging, not a
			// cursor nudge that scrolls the view by a single line.
			const int nStep  = (int) VisibleRows ();
			const int nCount = (int) m_Menu.GetCount ();
			const int nMaxTop = nCount > nStep ? nCount - nStep : 0;
			int nTop = (int) m_nTopVisible + nPage * nStep;
			if (nTop < 0)
			{
				nTop = 0;
			}
			if (nTop > nMaxTop)
			{
				nTop = nMaxTop;
			}
			if ((unsigned) nTop != m_nTopVisible
			    || m_nCursor != (unsigned) nTop)
			{
				m_nTopVisible = (unsigned) nTop;
				m_nCursor = (unsigned) nTop;
				bRedraw = TRUE;
			}
		}

		int nJump = m_nJumpTo;
		if (nJump >= 0)
		{
			m_nJumpTo = -1;
			if ((unsigned) nJump < m_Menu.GetCount ()
			    && (unsigned) nJump != m_nCursor)
			{
				m_nCursor = (unsigned) nJump;
				bRedraw = TRUE;
			}
		}

		if (bRedraw)
		{
			DrawMenu ();
		}

		if (m_bSelectPending)
		{
			m_bSelectPending = FALSE;
			// Arms chain-boot; a kernel that cannot be booted is fatal
			// inside BootSelection (doom + hang), never a silent return.
			BootSelection (m_nCursor);
		}

		m_Scheduler.Yield ();
	}

	m_Logger.Write (FromKernel, LogNotice, "Chain-booting ...");
	m_Scheduler.Sleep (1);		// let the last screen writes settle

	return ShutdownReboot;
}

void CKernel::KeyStatusHandlerRaw (unsigned char /*ucModifiers*/,
				   const unsigned char RawKeys[6])
{
	if (s_pThis == 0)
	{
		return;
	}

	// Edge detection: act only on keys present now but not in the previous
	// report, so a held key does not repeat-fire.
	for (unsigned i = 0; i < 6; i++)
	{
		unsigned char nKey = RawKeys[i];
		if (nKey == 0)
		{
			continue;
		}

		boolean bWasDown = FALSE;
		for (unsigned j = 0; j < 6; j++)
		{
			if (s_pThis->m_PrevRawKeys[j] == nKey)
			{
				bWasDown = TRUE;
				break;
			}
		}
		if (bWasDown)
		{
			continue;
		}

		if (nKey == HidKeyUp)
		{
			s_pThis->m_nMoveAccum--;
		}
		else if (nKey == HidKeyDown)
		{
			s_pThis->m_nMoveAccum++;
		}
		else if (nKey == HidKeyEnter || nKey == HidKeyKpEnter)
		{
			s_pThis->m_bSelectPending = TRUE;
		}
		else if (nKey == HidKeyPageUp)
		{
			s_pThis->m_nPageAccum--;
		}
		else if (nKey == HidKeyPageDown)
		{
			s_pThis->m_nPageAccum++;
		}
		else if (nKey >= HidKeyDigit1 && nKey <= HidKeyDigit9)
		{
			s_pThis->m_nJumpTo = (int) (nKey - HidKeyDigit1);
		}
	}

	memcpy (s_pThis->m_PrevRawKeys, RawKeys, sizeof s_pThis->m_PrevRawKeys);
}

void CKernel::KeyboardRemovedHandler (CDevice * /*pDevice*/, void * /*pContext*/)
{
	if (s_pThis != 0)
	{
		s_pThis->m_pKeyboard = 0;
	}
}
