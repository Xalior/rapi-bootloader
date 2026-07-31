//
// kernel.h
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2014-2019  R. Stange <rsta2@o2online.de>
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
#ifndef _kernel_h
#define _kernel_h

#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/screen.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/net/netsubsystem.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/types.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include "teedevice.h"

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
	// Acquire the loader's address: DHCP where a server answers, the baked
	// static configuration where none does. The wait is bounded (see
	// DHCP_WAIT_SECONDS) because a segment with no DHCP server is a normal
	// case here, not a fault — the boot must not stall on it.
	void AcquireNetworkConfig (void);

	// Quiesce the USB stack (pump plug-and-play until xHCI settles) before
	// any path that reboots. Circle's chain-boot/reboot teardown asserts
	// (ptrlist m_pFirst == 0) if it tears an unsettled xHCI controller
	// down — the menu-loader settles the same way before auto-boot.
	void QuiesceUSB (void);

private:
	// do not change this order
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CScreenDevice		m_Screen;
	CSerialDevice		m_Serial;
	CTeeDevice		m_Tee;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CScheduler		m_Scheduler;
	CUSBHCIDevice		m_USBHCI;
	CNetSubSystem		m_Net;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;
};

#endif
