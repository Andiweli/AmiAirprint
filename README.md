# AmiAirPrint 1.0

![AmigaOS](https://img.shields.io/badge/AmigaOS-3.0%2B-orange)
![Printing](https://img.shields.io/badge/Printing-AirPrint%20%2F%20IPP-blue)
![AI](https://img.shields.io/badge/Development-AI--assisted-6e7781)

**Native AirPrint / IPP printing for classic AmigaOS 3.0 and newer.**

AmiAirPrint integrates network printers with the standard AmigaOS `printer.device` system. Applications can print normal Amiga raster graphics and processed text without CUPS, a PC print server, or a printer-specific classic Amiga driver.

## Highlights

- AmigaOS 3.0+ and Motorola 68000 compatible
- standard `printer.device` integration
- direct IPP `Print-Job` transport through `airprint.device`
- PWG Raster output for normal Amiga graphics printing
- native processed text printing through `PRT:` / `CMD_WRITE`
- color and monochrome output (`sRGB 8` / `sGray 8` where supported)
- Portrait and Landscape
- selectable paper/media, print quality and color mode
- 10-100% raster scaling and optional centering on paper
- IPP printer capability query and JPEG test page
- AmigaOS 3.0/3.1 GadTools preferences program
- AmigaOS 3.2+ ReAction preferences program
- both preferences programs use the same settings file
- locale.library/catalog support with built-in English fallback

## Requirements

- AmigaOS 3.0 or newer
- a TCP/IP stack providing `bsdsocket.library`
- an IPP/AirPrint-capable network printer reachable by IPv4 address
- `printer.device` configured with the supplied `AirPrint` driver

AmiAirPrint currently uses a numeric IPv4 printer address. DNS/mDNS discovery is not part of version 1.0.

## Installation

The supplied Installer script is compatible with Installer 43.3.

1. Start `Install-AmiAirPrint` with Installer 43.3.
2. Select **AmigaOS 3.0 / 3.1** or **AmigaOS 3.2+**.
3. Restart the Amiga after installation.
4. Open the normal AmigaOS Printer preferences and select:
   - Printer: **AirPrint**
   - Port: **Parallel**
5. Open `SYS:Prefs/AmiAirPrint`, enter the printer IPv4 address, query the
   printer, select the desired options, and save.

`Parallel` is only a compatibility selection required by classic `printer.device`. AmiAirPrint does **not** send PWG/IPP payload data to `parallel.device`; the AirPrint driver opens `airprint.device` directly.

### Installed files

For AmigaOS 3.0/3.1 the Installer copies the GadTools preferences program as `SYS:Prefs/AmiAirPrint`. For AmigaOS 3.2+ it copies the ReAction version under the same name.

The shared driver files are installed as:

```text
SYS:DEVS/airprint.device
SYS:DEVS/Printers/AirPrint
```

Both preferences programs read and write:

```text
ENV:AirPrint.prefs
ENVARC:AirPrint.prefs
```

## Printing paths

### Graphics

```text
Application
  -> printer.device
  -> DEVS:Printers/AirPrint
  -> PWG Raster
  -> airprint.device
  -> IPP Print-Job
  -> printer
```

`printer.device` performs the normal Amiga source decoding, scaling and color conversion. AmiAirPrint converts the resulting rows to PWG Raster and streams them to the network device.

### Text

Processed `PRT:` / `CMD_WRITE` text is consumed by the printer driver's `ConvFunc`/`DoSpecial` hooks and rasterized with an embedded 8x8 Latin-1 font. The text path supports CR/LF, tabs, form feed, automatic line/page wrapping, 10/12/~17 CPI, 6/8 LPI, margins, bold, italic and underline.

## Preferences programs

- **AmiAirPrintPrefsClassic** - GadTools V39 for AmigaOS 3.0/3.1
- **AmiAirPrintPrefs** - ReAction for AmigaOS 3.2+

The Installer installs the correct binary as `SYS:Prefs/AmiAirPrint`.

The Workbench icon may retain `STACK=80000`; version 1.0 also keeps the large GUI and HTTP workspaces off the process stack, so the program no longer relies on that large stack for normal operation.


## Localization

Both preferences programs are prepared for `locale.library` V38+. English is always built in, so no catalog is required. If `AmiAirPrint.catalog` exists for the user's configured language, it is loaded automatically through the normal AmigaOS catalog search path (`PROGDIR:Catalogs/<language>/` and `LOCALE:Catalogs/<language>/`).

The release intentionally ships only the catalog description file:

```text
Locale/AmiAirPrint.cd
```

Translators can use CatComp, FlexCat (prefered or compatible tools to create their own `.ct` and `AmiAirPrint.catalog` files. With CatComp 40.x, for example:

```text
CatComp Locale/AmiAirPrint.cd CTFILE AmiAirPrint_deutsch.ct
CatComp Locale/AmiAirPrint.cd AmiAirPrint_deutsch.ct CATALOG AmiAirPrint.catalog
```

Message IDs in the `.cd` are explicit and stable; existing IDs must not be renumbered in future 1.x releases.

The AmigaOS 3.2+ ReAction preferences GUI uses `LAYOUT_SpaceInner` and `LAYOUT_SpaceOuter` instead of hard-coded inter-gadget pixel spacing, so the user's ReAction.prefs spacing is respected. The GadTools Classic GUI is not controlled by ReAction.prefs.

## Verified 1.0 printing paths

The 1.0 release has been regression-tested with:

- Personal Paint color graphics printing
- MultiView graphics printing
- IBrowse text-only printing on a white background
- AmiAirPrint's built-in monochrome test page

## Known limitations

- Printer discovery is not implemented; enter the printer's IPv4 address.
- IBrowse's classic printer output is text-only. Image placeholders such as `[Image alt=...]` are generated by IBrowse itself and are not a driver error.
- `PRD_RAWWRITE` is intentionally not treated as portable text. Raw output is printer-specific by definition.
- Duplex capability may be detected, but duplex selection is not exposed as a version 1.0 print option.

## Building

The project is built with `m68k-amigaos-gcc`:

```sh
make clean
make
```

All targets are compiled for `-m68000`. The Makefile enables `-Wframe-larger-than=1024` for all C targets so accidental large automatic stack frames are reported during development.

Normal end users only need:

- `AmiAirPrintPrefs` or `AmiAirPrintPrefsClassic`
- `airprint.device`
- `AirPrint`

The other binaries produced by the Makefile are diagnostics and development tools.

## License

AmiAirPrint is released under the MIT License. See `LICENSE`.

The embedded 8x8 Latin-1 bitmap font is derived from Daniel Hepper's `font8x8` collection and is Public Domain. See `THIRD-PARTY.md`.

Copyright (c) 2026 Andreas "Andiweli" Stuermer.
