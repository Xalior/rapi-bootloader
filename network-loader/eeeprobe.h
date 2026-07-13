//
// eeeprobe.h — PHY EEE state inspection and control over MDIO.
//
// Circle's genet driver leaves Energy-Efficient Ethernet entirely at PHY
// hardware defaults (it defines the UMAC EEE registers but never writes
// them, and never touches the PHY's EEE advertisement), so the PHY
// negotiates EEE with a willing link partner while the MAC knows nothing
// of LPI — and locally-originated UDP frames die on the boots where the
// partners actually use it. The kernel clears the advertisement at boot.
//
// These helpers talk MDIO directly through the GENET MDIO_CMD register —
// the same MMIO the driver uses, from the same (cooperative, core-0)
// context; the register Circle never touches stays ours to manage.
//
#ifndef _eeeprobe_h
#define _eeeprobe_h

#include <circle/types.h>

struct TEEEState
{
	int nBMSR;		// basic status
	int nLPA;		// link partner ability
	int nStat1000;		// 1000BASE-T status
	int nEEEAdv;		// MMD 7.0x3C: our EEE advertisement
	int nEEELpAbility;	// MMD 7.0x3D: link partner's EEE ability
	u32 nUmacEEECtrl;	// UMAC_EEE_CTRL MMIO
};

// Read the current EEE/link state (negative fields = MDIO read failed).
void EEEProbeRead (TEEEState *pState);

// Clear the PHY's EEE advertisement (MMD 7.0x3C = 0) and restart
// autonegotiation. Returns the advertisement read back after clearing.
int EEEProbeDisable (void);

#endif
