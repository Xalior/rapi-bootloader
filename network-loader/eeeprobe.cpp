//
// eeeprobe.cpp — PHY EEE state inspection and control over MDIO.
//
// MDIO mechanics mirror circle/lib/bcm54213.cpp (GPLv3): GENET MDIO_CMD
// at UMAC offset 0x614, START_BUSY-triggered transactions, PHY address 1
// (external BCM54213PE on the Pi 4). Clause-22 MMD indirection through
// registers 13/14 reaches the EEE registers in MMD 7 (autonegotiation).
//
// The GENET/BCM54213 MAC+PHY pairing is Pi 4 only — Circle's bcm2711.h
// defines ARM_BCM54213_BASE under `#if RASPPI == 4` and nowhere else, so
// this probe compiles to nothing on Pi 3/5 (see the matching `#if RASPPI
// == 4` around its call site in kernel.cpp).
//
#include "eeeprobe.h"

#if RASPPI == 4

#include <circle/bcm2711.h>
#include <circle/memio.h>
#include <circle/timer.h>

#define GENET_UMAC_OFF		0x0800
#define UMAC_MDIO_CMD		0x614
#define UMAC_EEE_CTRL		0x064

#define MDIO_START_BUSY		(1 << 29)
#define MDIO_READ_FAIL		(1 << 28)
#define MDIO_RD			(2 << 26)
#define MDIO_WR			(1 << 26)
#define MDIO_PMD_SHIFT		21
#define MDIO_REG_SHIFT		16

#define PHY_ADDR		0x01

#define MII_BMCR		0x00
#define MII_BMSR		0x01
#define MII_LPA			0x05
#define MII_STAT1000		0x0A
#define MII_MMD_CTRL		0x0D
#define MII_MMD_DATA		0x0E

#define BMCR_ANRESTART		(1 << 9)

#define MMD_AN			7
#define MMD_AN_EEE_ADV		0x3C
#define MMD_AN_EEE_LPABLE	0x3D

static inline u32 mdio_cmd_read (void)
{
	return read32 (ARM_BCM54213_BASE + GENET_UMAC_OFF + UMAC_MDIO_CMD);
}

static inline void mdio_cmd_write (u32 nValue)
{
	write32 (ARM_BCM54213_BASE + GENET_UMAC_OFF + UMAC_MDIO_CMD, nValue);
}

static int mdio_wait (void)
{
	unsigned nStart = CTimer::Get ()->GetTicks ();
	while (mdio_cmd_read () & MDIO_START_BUSY)
	{
		if (CTimer::Get ()->GetTicks () - nStart > HZ)
		{
			return -1;
		}
	}

	return 0;
}

static int mdio_read (int nReg)
{
	mdio_cmd_write (MDIO_RD | (PHY_ADDR << MDIO_PMD_SHIFT) | (nReg << MDIO_REG_SHIFT));
	mdio_cmd_write (mdio_cmd_read () | MDIO_START_BUSY);
	if (mdio_wait () < 0)
	{
		return -1;
	}

	u32 nCmd = mdio_cmd_read ();
	if (nCmd & MDIO_READ_FAIL)
	{
		return -1;
	}

	return nCmd & 0xFFFF;
}

static int mdio_write (int nReg, u16 nValue)
{
	mdio_cmd_write (MDIO_WR | (PHY_ADDR << MDIO_PMD_SHIFT) | (nReg << MDIO_REG_SHIFT) | nValue);
	mdio_cmd_write (mdio_cmd_read () | MDIO_START_BUSY);

	return mdio_wait ();
}

// Clause-22 indirect access to an MMD register.
static int mmd_read (int nDevAd, int nReg)
{
	if (   mdio_write (MII_MMD_CTRL, nDevAd) < 0		// address function
	    || mdio_write (MII_MMD_DATA, nReg) < 0
	    || mdio_write (MII_MMD_CTRL, 0x4000 | nDevAd) < 0)	// data function
	{
		return -1;
	}

	return mdio_read (MII_MMD_DATA);
}

static int mmd_write (int nDevAd, int nReg, u16 nValue)
{
	if (   mdio_write (MII_MMD_CTRL, nDevAd) < 0
	    || mdio_write (MII_MMD_DATA, nReg) < 0
	    || mdio_write (MII_MMD_CTRL, 0x4000 | nDevAd) < 0)
	{
		return -1;
	}

	return mdio_write (MII_MMD_DATA, nValue);
}

void EEEProbeRead (TEEEState *pState)
{
	pState->nBMSR = mdio_read (MII_BMSR);
	pState->nLPA = mdio_read (MII_LPA);
	pState->nStat1000 = mdio_read (MII_STAT1000);
	pState->nEEEAdv = mmd_read (MMD_AN, MMD_AN_EEE_ADV);
	pState->nEEELpAbility = mmd_read (MMD_AN, MMD_AN_EEE_LPABLE);
	pState->nUmacEEECtrl = read32 (ARM_BCM54213_BASE + GENET_UMAC_OFF + UMAC_EEE_CTRL);
}

int EEEProbeDisable (void)
{
	mmd_write (MMD_AN, MMD_AN_EEE_ADV, 0);

	int nReadback = mmd_read (MMD_AN, MMD_AN_EEE_ADV);

	// Renegotiate so the cleared advertisement takes effect.
	int nBMCR = mdio_read (MII_BMCR);
	if (nBMCR >= 0)
	{
		mdio_write (MII_BMCR, nBMCR | BMCR_ANRESTART);
	}

	return nReadback;
}

#endif // RASPPI == 4
