//
// teedevice.h
//
// Write-only fan-out device: every Write() is forwarded to two backing
// devices. The network-loader points its CLogger here so one logger feeds
// both the HDMI screen (readable at the glass and via hdmi-grab) and the
// serial UART (captured remotely by the host serial logger). This is the
// same serial-diagnostics split the menu-loader and the MAME payload use
// (m_Logger.Initialize (&m_Serial)), teed so the screen stays populated
// too — a chainloader-phase panic then lands on serial instead of being
// visible only on the glass.
//
// GPLv3, consistent with the rest of this loader.
//
#ifndef _teedevice_h
#define _teedevice_h

#include <circle/device.h>
#include <circle/types.h>

class CTeeDevice : public CDevice
{
public:
	// pPrimary governs the return value (the screen, which never stalls);
	// pSecondary is best-effort (the UART), so a wedged serial line can
	// never hold up logging. Either may be null.
	CTeeDevice (CDevice *pPrimary, CDevice *pSecondary)
	:	m_pPrimary (pPrimary),
		m_pSecondary (pSecondary)
	{
	}

	int Write (const void *pBuffer, size_t nCount) override
	{
		if (m_pSecondary != 0)
		{
			m_pSecondary->Write (pBuffer, nCount);
		}

		if (m_pPrimary != 0)
		{
			return m_pPrimary->Write (pBuffer, nCount);
		}

		return (int) nCount;
	}

private:
	CDevice *m_pPrimary;
	CDevice *m_pSecondary;
};

#endif
