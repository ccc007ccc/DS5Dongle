# M61 Windows one-file flasher

This is the single-file graphical Windows flasher intended for normal users.
Double-clicking the Release EXE opens only the GUI; no command prompt is
required. The EXE
embeds Bouffalo `BLFlashCommand.exe`, then lists complete firmware versions from
the project's GitHub Releases so the user can select, download, verify, and
flash one. Python, Rust, the SDK, and a compiler are not required on the user's
PC.

The GUI defaults to Simplified Chinese for a Chinese Windows user locale and
English otherwise. The language selector in the top-right corner can switch
the complete interface at any time.

At runtime it:

1. verifies the SHA256 of the embedded flashing tool;
2. lists Releases containing one complete `M61-Firmware-<version>.zip`;
3. verifies the ZIP against GitHub's digest, then verifies its complete flash set
   and internal checksum manifest;
4. detects the CH340 `USB\VID_1A86&PID_7523` device and COM port;
5. downloads the driver over HTTPS from WCH only when the hardware has no usable
   COM port;
6. verifies the Authenticode signer before requesting UAC for the official
   installer;
7. guides the user through reliable BOOT+RESET UART ISP entry;
8. flashes at 460800 baud by default, with an optional 115200-baud retry.

The GUI supports online Release ZIPs, local firmware ZIPs, and an advanced
complete-firmware directory mode. ZIP parsing rejects unsafe paths, duplicates,
oversized files, invalid partition size, and checksum mismatches. Logs are
mouse-selectable and also have a Copy all button; the ISP warning is red.

See [README.zh-CN.md](README.zh-CN.md) for local build and test commands.
`--verify-release` downloads and checks a selected firmware set without opening
a COM port or starting a flash operation.
`--tool-preflight` additionally proves that the embedded `chips/bl616` support
files reach BL616 loader initialization while using a deliberately nonexistent
COM port.
The generated `eflash_loader_cfg.ini` starts from the SDK's tracked `.conf`
template and normalizes it to LF, so clean CI checkouts do not depend on
ignored state from a previous flash run or Git line-ending settings.
Use the debug build for these developer CLI checks. The Release build uses the
Windows GUI subsystem and opens no command-prompt window when double-clicked.
