# Dependencies, licensing, and redistribution

[简体中文](OPEN_SOURCE.zh-CN.md)

## Repository boundary

This repository intentionally contains only M61 application source, build
metadata, tests, and maintained documentation. It does not vendor the
1+ GiB Bouffalo SDK, compiler binaries, generated firmware, benchmark logs, or
vendor PDF manuals.

The SDK audit found one historical four-line linker-script edit and no local
CherryUSB, FreeRTOS, or Bluetooth library source changes. That memory choice
is now expressed as project-level `CONFIG_WRAM_LENGTH=163840`, so the locked
official SDK checkout remains clean. A separate SDK fork would add more than
one gigabyte and no longer carries a required modification, so it is neither
needed nor maintained.

Opus is different: its performance changes modify codec source. The project
therefore stores small reviewable patch files, downloads the exact upstream
1.2.1 archive by SHA256, and builds it locally. This preserves both
reproducibility and upstream license notices without vendoring a generated
source tree.

## License map

| Material | Location/source | License |
| --- | --- | --- |
| M61 application, tools, maintained docs | This repository | MIT |
| Original DS5Dongle work | `awalol/DS5Dongle` history | MIT |
| Bouffalo SDK | External locked checkout | Apache-2.0 plus component licenses |
| CherryUSB | Through Bouffalo SDK | Apache-2.0 |
| FreeRTOS | Through Bouffalo SDK | MIT |
| Opus 1.2.1 and M61 patches | Downloaded source plus tracked patches | Opus BSD-style license/patent notices |
| Sony names/report formats | Interoperability identifiers | No Sony code, firmware, keys, or artwork is distributed |

See [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) before distributing
a firmware binary. A binary distributor must include applicable object-form
notices, especially the Opus copyright/license text.

## Vendor documents

Datasheets and board manuals are useful development references but may have
separate redistribution terms. PDF copies are ignored under `docs/vendor/`
and are not committed. Public documentation records only the facts needed to
build and wire the project; users should obtain current manuals from the
vendor.

## Release checklist

Publish:

- source commit/tag;
- BIN, ELF, and MAP when appropriate;
- generated `.manifest.json`;
- MIT license and `THIRD_PARTY_NOTICES.md`;
- bilingual release notes describing the exact hardware and profile.

Do not publish:

- SDK/toolchain binaries copied into this repository;
- Opus build/cache directories;
- private controller data or pairing keys;
- local COM-port logs, absolute paths, or vendor PDFs without permission;
- a custom/unverified manifest labeled as release.
