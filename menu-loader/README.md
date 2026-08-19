# menu-loader

menu-loader is an on-card boot picker for bare-metal Raspberry Pi payloads.
Firmware boots it from the SD card. It reads a `bootmenu.cfg` list from the
card and shows it on screen, takes a keyboard selection, stamps the chosen
argv [defaults-string](../README.md#the-0x800-defaults-block-abi) into a
staged kernel image, and chain-boots it.

Part of [rapi-bootloader](../README.md). Build with `make menu-loader` from
the repo root, after `make deps`. It builds one image per board into
`menu-loader/build/<board>/`, using Circle's own names: `kernel8.img`,
`kernel8-rpi4.img` and `kernel_2712.img` for RASPPI 3, 4 and 5. The loader
is single-core by design. Circle's `EnableChainBoot()` refuses a multicore
build, and the Raspberry Pi 5 chain-boot in
[`chainboot/`](../chainboot/) keeps the same restriction.

## On-card files

Both paths are on the SD card the firmware boots this loader from:

- `bootmenu.cfg` is the menu (format below).
- `kernel-<board>.img` is the single staged kernel image every entry boots.
  `<board>` is the board tag derived at compile time from RASPPI (`rpi3`,
  `rpi4` or `rpi5`), so on a Pi 4 build the file is `kernel-rpi4.img`. This
  lets one card carry a payload per board. No menu entry carries its own
  image; each one stamps its chosen defaults-string into this same staged
  image.

## bootmenu.cfg

One entry per line, `Label | defaults-string`:

```
# lines starting with '#' are comments; blank lines are skipped
First entry           | string-one
Second entry (opts)   | string-two --flag ""
Third entry           | string-three
```

- The loader splits each line on the first `|`. A line without one is
  ignored, and logged.
- The label, before the `|`, is shown in the menu. The string, after it, is
  the argv text stamped into the staged image's 0x800 block for that entry.
  The loader boots an image carrying no 0x800 block unstamped rather than
  refusing it, exactly as network-loader does, because both use the same
  writer.
- The loader trims whitespace around the `|`, and a trailing CR, so CRLF
  files work fine.
- Limits: up to 64 entries, label ≤ 96 bytes, string ≤ 1024 bytes; anything
  beyond is truncated or ignored (logged).

A single-entry `bootmenu.cfg` boots that one entry. With no second choice
on offer, the picker becomes a fixed, unattended chain-boot of one baked
entry.

## Keys

| Key | Action |
|-----|--------|
| Up / Down     | move the selection |
| PgUp / PgDn   | page through a long list |
| `1`–`9`       | jump to that entry |
| Enter         | boot the selected entry |

The boot world always builds with the serial port on, so the same actions
also take one ASCII byte at a time on the debug UART: `j`/`k` move the
selection, `f`/`b` page down/up, `1`-`9` jump to an entry, and CR or LF
boots it. A serial console can script the menu this way, with no keyboard
attached.

## Keyboard layout

The menu reads the keyboard as raw HID usage codes, not as characters, so it is
unaffected by the keyboard layout. The cursor keys, the digits, Enter and the
paging keys carry the same codes on every layout, and the menu behaves
identically whatever the card says.

The card's `cmdline.txt` still carries a layout, because the payload booted from
the menu may want one:

    keymap=UK

Valid values are `US`, `UK`, `DE`, `ES`, `FR`, `IT` and `DV` (Dvorak). A
missing or unrecognised name falls back to German. Names are case-sensitive.
A name in the wrong case matches nothing, and the system falls back to
German instead. It matters to anything that reads typed characters rather
than named keys. On the wrong layout the letters and digits are still
right, and the punctuation is not.
