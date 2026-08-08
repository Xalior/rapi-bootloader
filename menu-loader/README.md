# menu-loader

An on-card boot picker for bare-metal Raspberry Pi payloads. Firmware boots
it from the SD card; it reads a `bootmenu.cfg` list from the card, presents it
on screen, takes a keyboard selection, stamps the chosen argv
[defaults-string](../README.md#the-0x800-defaults-block-abi) into a staged
kernel image, and chain-boots it.

Part of [rapi-bootloader](../README.md). Build with `make menu-loader` from the
repo root (after `make deps`); it builds one image per board into
`menu-loader/build/<board>/` (Circle's own names — `kernel8.img` / `kernel8-rpi4.img`
/ `kernel_2712.img` for RASPPI 3 / 4 / 5). Single-core by design — Circle's
`EnableChainBoot()` refuses a multicore build, and the Raspberry Pi 5
chain-boot in [`chainboot/`](../chainboot/) keeps the same restriction.

## On-card files

Both paths are on the SD card the firmware boots this loader from:

- `bootmenu.cfg` — the menu (format below).
- `kernel-<board>.img` — the single staged kernel image every entry boots
  (`<board>` is the board tag derived at compile time from RASPPI: `rpi3` /
  `rpi4` / `rpi5`, so e.g. `kernel-rpi4.img`; it lets one card carry a payload
  per board). Each menu entry does not carry its own image: they all stamp
  their chosen defaults-string into this one staged image.

## bootmenu.cfg

One entry per line, `Label | defaults-string`:

```
# lines starting with '#' are comments; blank lines are skipped
First entry           | string-one
Second entry (opts)   | string-two --flag ""
Third entry           | string-three
```

- Split on the **first** `|`; a line without one is ignored (logged).
- The label (before the `|`) is shown in the menu; the string (after it) is
  the argv text stamped into the staged image's 0x800 block for that entry.
  An image that carries no 0x800 block boots unstamped rather than being
  refused, exactly as on the network-loader — both use the same writer.
- Whitespace around the `|` is trimmed, as is a trailing CR (CRLF files are
  fine).
- Limits: up to **64** entries, label ≤ 96 bytes, string ≤ 1024 bytes;
  anything beyond is truncated or ignored (logged).

A single-entry `bootmenu.cfg` boots that entry — the picker is then just a
fixed, unattended chain-boot of one baked entry.

## Keys

| Key | Action |
|-----|--------|
| Up / Down     | move the selection |
| PgUp / PgDn   | page through a long list |
| `1`–`9`       | jump to that entry |
| Enter         | boot the selected entry |

## Keyboard layout

The menu reads the keyboard as raw HID usage codes, not as characters, so it is
unaffected by the keyboard layout. The cursor keys, the digits, Enter and the
paging keys carry the same codes on every layout, and the menu behaves
identically whatever the card says.

The card's `cmdline.txt` still carries a layout, because the payload booted from
the menu may want one:

    keymap=uk

Valid values are `us` (the default), `uk`, `de`, `es`, `fr`, `it` and `dv`
(Dvorak). It matters to anything that reads typed characters rather than named
keys; on the wrong layout the letters and digits are still right and the
punctuation is not.
