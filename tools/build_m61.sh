#!/usr/bin/env bash
# Build the M61 (BL616/BL618) DualSense bridge firmware from WSL.
#
# Usage: tools/build_m61.sh [single|dual]
#   single — Bluetooth on the M61 itself (defconfig)
#   dual   — ESP32 SPI coprocessor profile (defconfig.dual_chip, default)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJ="$ROOT/m61/dualsense_hidp_probe"
VARIANT="${1:-dual}"
CROSS="${CROSS_COMPILE:-/opt/toolchain_gcc_t-head_linux/bin/riscv64-unknown-elf-}"

cd "$PROJ"
case "$VARIANT" in
    single) src=defconfig ;;
    dual)   src=defconfig.dual_chip ;;
    *) echo "unknown variant: $VARIANT" >&2; exit 1 ;;
esac

if [ "$src" != defconfig ]; then
    cmp -s "$src" defconfig.active 2>/dev/null || rm -rf build
    cp "$src" defconfig.active
    cp "$src" defconfig.build_tmp
    # bl_mcu_sdk only reads ./defconfig — keep the original in git, swap for build
    cp defconfig defconfig.single_backup
    cp "$src" defconfig
    trap 'cp defconfig.single_backup defconfig; rm -f defconfig.single_backup defconfig.build_tmp' EXIT
fi

make -j"$(nproc)" CHIP=bl616 BOARD=bl616dk CROSS_COMPILE="$CROSS"
echo "firmware: $PROJ/build/build_out/m61_dualsense_hidp_probe_bl616.bin"
