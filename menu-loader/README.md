# menu-loader

An on-card boot picker for bare-metal Raspberry Pi 4 payloads. Firmware boots
it from the SD card; it reads a `bootmenu.cfg` list from the card, presents it
on screen, takes a keyboard selection, stamps the chosen argv
[defaults-string](../README.md#the-0x800-defaults-block-abi) into a staged
platform kernel image, and chain-boots it.

Part of [rapi-bootloader](../README.md). Build with `make menu-loader` from the
repo root (after `make deps`); the image is `menu-loader/kernel8-rpi4.img`.
Single-core by design — Circle's `EnableChainBoot()` refuses a multicore build.

## On-card files

Both paths are on the SD card the firmware boots this loader from:

- `bootmenu.cfg` — the menu (format below).
- `pi-mame-core-<board>.img` — the single platform kernel image every entry
  boots (`<board>` is the target board tag, currently `rpi4`, so
  `pi-mame-core-rpi4.img`; it lets one card carry a payload per board). Each
  menu entry does not carry its own image: they all stamp their chosen
  defaults-string into this one staged image.

## bootmenu.cfg

One entry per line, `Label | defaults-string`:

```
# lines starting with '#' are comments; blank lines are skipped
Commodore 64          | c64
Commodore 64 (+ IEC)  | c64 -iec8 ""
VIC-20                | vic20
```

- Split on the **first** `|`; a line without one is ignored (logged).
- The label (before the `|`) is shown in the menu; the string (after it) is
  the argv text stamped into the staged image's 0x800 block for that entry.
- Whitespace around the `|` is trimmed, as is a trailing CR (CRLF files are
  fine).
- Limits: up to **64** entries, label ≤ 96 bytes, string ≤ 1024 bytes;
  anything beyond is truncated or ignored (logged).

A single-entry `bootmenu.cfg` boots that entry — the picker is then just a
fixed, unattended chain-boot of one baked machine.

## Keys

| Key | Action |
|-----|--------|
| Up / Down     | move the selection |
| PgUp / PgDn   | page through a long list |
| `1`–`9`       | jump to that entry |
| Enter         | boot the selected entry |
