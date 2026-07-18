# Building and flashing

[简体中文](BUILDING.zh-CN.md)

Normal users do not need this build environment. Download
`M61-Flasher-Windows.exe` from the project
[Releases](https://github.com/ccc007ccc/DS5Dongle/releases), double-click it,
and select a firmware version. It detects CH340, downloads and verifies the
official WCH driver only when needed, verifies the selected complete Release
ZIP, and guides reliable BOOT+RESET flashing. You can also download
`M61-Firmware-<version>.zip` first and select Local firmware ZIP in the GUI for
offline flashing.

## Supported release environment

The release build is defined by
[`reproducible-build.lock.json`](../m61/dualsense_hidp_probe/reproducible-build.lock.json),
not by whichever SDK or compiler happens to be installed.

| Dependency | Locked revision |
| --- | --- |
| Bouffalo SDK | `2.3.24`, `d9306a4a221db414131337ec95113e3adaf7072b` |
| Windows T-Head toolchain | `072fc29d765774d66366c57a4d962e90c366ef1b` |
| Linux T-Head toolchain | `c4afe91cbd01bf7dce525e0d23b4219c8691e8f0` |
| GCC identity | Xuantie-900 V2.6.1 B-20220906, GCC 10.2.0 |
| Opus | 1.2.1 archive SHA256 `cfafd339...70732` plus 11 locked patches |

Python 3, Git, CMake/Ninja/Make from the SDK, and `tar` are also required.
Windows 10/11 and PowerShell 7 are the primary tested host.

## Manually flashing Release files (developer fallback)

To skip compilation, choose one
[Release](https://github.com/ccc007ccc/DS5Dongle/releases) that lists the
complete flash set, then download boot2, partition, the application BIN, and
the matching `flash-files.sha256`. All three BIN files
must come from the same version and be placed under:

```text
m61/dualsense_hidp_probe/build-win/build_out/
  boot2_bl616_isp_release_v8.1.8.bin
  partition.bin
  m61_dualsense_hidp_probe_bl616.bin
```

Verify the SHA256 values, then follow the flashing section with
`--windows-build`. This path needs neither a compiler nor an Opus build, but
the locked `bl_mcu_sdk` must still be cloned next to the repository because it
provides the flashing tool.

Each formal Release also packages those files and the checksum manifest as
`M61-Firmware-<version>.zip`. Online downloads and local ZIP installation use
the same safe parser and verification path. Files may be at the ZIP root or in
one subdirectory, but exactly one boot2, partition, and application BIN must be
present.

Maintainers can reproduce the archive with:

```powershell
python tools\package_m61_firmware_zip.py `
  --input-dir C:\path\to\release-files `
  --tag v0.8.1 `
  --output M61-Firmware-v0.8.1.zip
```

The script verifies the checksum manifest first, then writes a fixed entry
order, timestamp, and permissions so repeated runs with the same inputs produce
the same file.

## Windows release build

```powershell
git clone https://github.com/ccc007ccc/DS5Dongle.git
git clone https://github.com/bouffalolab/bl_mcu_sdk.git
git -C bl_mcu_sdk checkout d9306a4a221db414131337ec95113e3adaf7072b
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_windows.git
git -C toolchain_gcc_t-head_windows checkout 072fc29d765774d66366c57a4d962e90c366ef1b

cd DS5Dongle\m61\dualsense_hidp_probe
.\build_windows.ps1 -Command All -SdkPath C:\work\bl_mcu_sdk -ToolchainBin C:\work\toolchain_gcc_t-head_windows\bin
```

The first build downloads the Opus archive, verifies it, extracts a fresh
tree, applies every patch in the locked order, and builds the optimized
library. No modified SDK checkout is required.

The dependency gate checks:

1. exact SDK commit and a clean SDK worktree;
2. exact toolchain commit, compiler identity, and Windows compiler SHA256;
3. every Opus patch SHA256;
4. the release configuration embedded in the generated manifest.

## Linux release build

```bash
git clone https://github.com/ccc007ccc/DS5Dongle.git
git clone https://github.com/bouffalolab/bl_mcu_sdk.git
git -C bl_mcu_sdk checkout d9306a4a221db414131337ec95113e3adaf7072b
git clone https://github.com/bouffalolab/toolchain_gcc_t-head_linux.git
git -C toolchain_gcc_t-head_linux checkout c4afe91cbd01bf7dce525e0d23b4219c8691e8f0

cd DS5Dongle/m61/dualsense_hidp_probe
BL_SDK_BASE=/work/bl_mcu_sdk M61_TOOLCHAIN_BIN=/work/toolchain_gcc_t-head_linux/bin ./build.sh all
```

Do not share GCC LTO archives between Windows and Linux hosts. Each host
builds its own archive from the same source and options.

## Locked release profile

| Setting | Release value |
| --- | --- |
| App target | BL616 / `bl616dk` |
| WRAM partition | 160 KiB |
| USB/codec translation unit | `-O2` |
| Opus | fixed point, `-O2 -flto`, E907 patch profile |
| Opus placement | `pvq-mdct-decode-mdct` |
| Codec paired service window | 1 ms |
| CRC | 16-entry Flash nibble table |
| Compile-time overclock | off |
| Profiling | all off |
| Runtime defaults | manual 320 MHz, mic off, speaker route auto |

Diagnostic flags such as `-HpmProfile`, `-PipelineProfile`,
`-RuntimeProfile`, `-OpusStageProfile`, or a non-default Opus placement
create a custom build. They are useful for measurement but add overhead or
change code layout.

`-AllowUnverifiedDependencies` / `--allow-unverified-dependencies` exists
for development only. Such a build must not be published as a
performance-equivalent release.

## Provenance manifest

Every successful build writes
`m61_dualsense_hidp_probe_bl616.manifest.json` beside the BIN/ELF/MAP. It
records source and dependency commits, all performance settings, the lock
hash, patch hashes, the effective `SOURCE_DATE_EPOCH`, and artifact SHA256
values. Publish the manifest together with a firmware binary.

The build scripts default `SOURCE_DATE_EPOCH` to the source commit timestamp.
This makes the SDK's `__DATE__`/`__TIME__` board and driver strings stable
instead of embedding the local wall clock. Two consecutive clean builds must
produce identical BIN and ELF SHA256 values. The manifest is authoritative
when comparing builds made on different machines. Overriding the epoch is
allowed for development but marks the manifest as `custom`.

## Flashing

Normal users run `M61-Flasher-Windows.exe`, select a firmware from its list,
and follow the on-screen steps. It uses 460800 baud by default and offers a
115200-baud compatibility retry after a failure.

The command below is the source-developer fallback. Enter ISP by holding BOOT,
tapping and releasing RESET, then releasing BOOT.

```powershell
python tools\flash_m61_firmware.py -p COM5 --windows-build
```

The default 460800 baud mode is the normal fast path. Select the M61 CH340
UART (`1A86:7523`); if Windows shows multiple COM ports, do not use a port in
an error state. Use `-b 115200` only as a conservative cable/hub fallback.

For a running compatible firmware, `--reboot-isp` can be tried as a best-effort
shortcut. Some BL616 boards enter BootROM but fail its eFuse read after a warm
reset, so physical BOOT+RESET remains the only reliable entry method. After
writing, release BOOT and reset into normal boot if the board remains in ISP.

## Verification

```powershell
python tools\run_offline_checks.py
python tools\check_m61_realtime_memory.py m61\dualsense_hidp_probe\build-win\build_out\m61_dualsense_hidp_probe_bl616.elf
python tools\check_m61_usb_windows.py
python tools\validate_m61_usb_hardware.py -p COM5
```

Serial capture and validation tools are development diagnostics. Close them
before measuring game input or audio, and never leave `ds5 log normal` enabled
during a throughput or stability run.

Hardware performance qualification uses the fixed 90-second load described
in [Performance](PERFORMANCE.md).
