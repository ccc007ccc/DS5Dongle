# Building and flashing

[简体中文](BUILDING.zh-CN.md)

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
hash, patch hashes, and artifact SHA256 values. Publish the manifest together
with a firmware binary.

Two consecutive builds in the same checkout have been verified to produce
identical BIN and ELF SHA256 values. The manifest is authoritative when
comparing builds made on different machines.

## Flashing

Manual ISP entry: hold BOOT, tap and release RESET, then release BOOT.

```powershell
python tools\flash_m61_firmware.py --app hidp-probe -p COM5 --windows-build
```

For a running compatible firmware, add `--reboot-isp`. After writing, release
BOOT and reset into normal boot if the board remains in ISP mode.

## Verification

```powershell
python tools\run_offline_checks.py
python tools\check_m61_realtime_memory.py m61\dualsense_hidp_probe\build-win\build_out\m61_dualsense_hidp_probe_bl616.elf
python tools\check_m61_usb_windows.py
python tools\validate_m61_usb_hardware.py -p COM5
```

Hardware performance qualification uses the fixed 90-second load described
in [Performance](PERFORMANCE.md).
