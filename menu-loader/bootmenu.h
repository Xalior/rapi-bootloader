//
// bootmenu.h
//
// Parses bootmenu.cfg from the card's FAT root into an ordered list of
// entries. One option per line: "label|defaults-string". The label is what
// the user sees; the string after the first '|' is what gets patched into
// the platform kernel's defaults block. Blank lines and lines beginning with
// '#' are ignored. Entries are kept in FILE ORDER (EASY mode: the top line is
// the default the cursor first rests on).
//
#ifndef _bootmenu_h
#define _bootmenu_h

#include <circle/types.h>

class CBootMenu
{
public:
	CBootMenu (void);
	~CBootMenu (void);

	// Read and parse the config file (e.g. "SD:/bootmenu.cfg"). Returns
	// FALSE if the file cannot be opened; a parsed file with no usable
	// entries returns TRUE with GetCount() == 0.
	boolean Load (const char *pPath);

	unsigned GetCount (void) const;
	const char *GetLabel (unsigned nIndex) const;	// 0 if out of range
	const char *GetString (unsigned nIndex) const;	// 0 if out of range

private:
	// Dead-simple fixed ceilings — a boot picker, not a database. An
	// over-long defaults-string is preserved up to MaxString so the patcher
	// can still refuse it against the block's real capacity, rather than a
	// silent truncation changing what the user asked for.
	static const unsigned MaxEntries = 64;
	static const unsigned MaxLabel	 = 96;
	static const unsigned MaxString	 = 1024;

	unsigned m_nCount;
	char m_Label[MaxEntries][MaxLabel];
	char m_String[MaxEntries][MaxString];

	void ParseLine (const char *pLine, size_t nLen);
};

#endif
