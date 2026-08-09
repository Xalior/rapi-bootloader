//
// netconfig.h
//
// The loader's network settings, read from the SD card's config.txt.
//
// config.txt is the Raspberry Pi firmware's own boot configuration file, and
// it is divided into sections by lines like [pi5] or [all]. The firmware
// treats a section whose name it does not recognise as a condition that is
// never true, and skips everything in it -- so a [rapi-bootloader] section is
// inert to the firmware and is a safe place to keep settings of our own in the
// one file a user already edits.
//
// A static address is used when the section supplies one, and DHCP when it
// does not. There is no built-in address: a loader that cannot be reached at a
// predictable place is no use to anyone, so the address is either stated on
// the card or asked for on the network.
//
#ifndef _netconfig_h
#define _netconfig_h

#include <circle/types.h>

struct TRapiNetConfig
{
	// FALSE when config.txt names no address: the caller uses DHCP.
	boolean	bStatic;

	u8	IPAddress[4];
	u8	NetMask[4];

	// A gateway is optional -- a loader reached from its own segment needs
	// none. FALSE leaves it unset.
	boolean	bHaveGateway;
	u8	Gateway[4];

	// A resolver is optional in the same way. A static address is the one
	// case where nothing supplies one: Circle's DHCP client fills the
	// resolver in along with the address, and a configuration written by
	// hand has only what the card states. FALSE leaves it unset, and
	// anything that then looks a name up has nowhere to ask.
	boolean	bHaveDNSServer;
	u8	DNSServer[4];
};

// Read the [rapi-bootloader] section of SD:/config.txt into *pConfig. The
// card must already be mounted. Always fills pConfig: on a missing file,
// missing section or missing address it returns with bStatic FALSE, which
// means DHCP.
void RapiReadNetConfig (TRapiNetConfig *pConfig);

#endif
