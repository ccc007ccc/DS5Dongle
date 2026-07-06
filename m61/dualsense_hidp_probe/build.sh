#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_PATH="${BL_SDK_BASE:-}"

if [[ -z "$SDK_PATH" ]]; then
    SDK_PATH="$(cd "$PROJECT_DIR/../../../bl_mcu_sdk" && pwd)"
fi

CHIP="bl616"
BOARD="bl616dk"
CPU_ID=""
COMMAND="build"
PROFILE="none"
BUILD_DIR_NAME="build"

log() {
    printf '[m61-hidp-build] %s\n' "$*"
}

fail() {
    printf '[m61-hidp-build] ERROR: %s\n' "$*" >&2
    exit 1
}

show_help() {
    cat <<'EOF'
Usage: ./build.sh [build|clean|all] [--chip bl616] [--board bl616dk] [--cpu-id ap] [--profile none|dual-chip-left-spi]

Builds the M61 DualSense Classic Bluetooth HIDP probe.

Profiles:
  none                Default config; dual-chip SPI transport stays disabled.
  dual-chip-left-spi  Build with the documented M61 left-side SPI pins enabled.

Environment:
  BL_SDK_BASE       Optional Bouffalo SDK path.
  M61_TOOLCHAIN_BIN Optional T-HEAD toolchain bin directory.

Example:
  ./build.sh
  ./build.sh all
  ./build.sh all --profile dual-chip-left-spi
EOF
}

find_toolchain_bin() {
    local candidates=()

    if [[ -n "${M61_TOOLCHAIN_BIN:-}" ]]; then
        candidates+=("$M61_TOOLCHAIN_BIN")
    fi

    candidates+=(
        "$HOME/riscv-toolchain/toolchain_gcc_t-head_linux/bin"
        "/home/ccc007/riscv-toolchain/toolchain_gcc_t-head_linux/bin"
        "/opt/toolchain_gcc_t-head_linux/bin"
    )

    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -x "$candidate/riscv64-unknown-elf-gcc" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

clean_build() {
    local project_real build_real

    project_real="$(realpath "$PROJECT_DIR")"
    if [[ -d "$PROJECT_DIR/$BUILD_DIR_NAME" ]]; then
        build_real="$(realpath "$PROJECT_DIR/$BUILD_DIR_NAME")"
        case "$build_real" in
            "$project_real"/"$BUILD_DIR_NAME")
                rm -rf "$build_real"
                ;;
            *)
                fail "refusing to remove unexpected build path: $build_real"
                ;;
        esac
    fi
}

build_project() {
    [[ -d "$SDK_PATH" ]] || fail "BL_SDK_BASE not found: $SDK_PATH"

    local toolchain_bin
    toolchain_bin="$(find_toolchain_bin)" || fail "T-HEAD riscv64-unknown-elf-gcc not found"

    export BL_SDK_BASE="$SDK_PATH"
    export PATH="$toolchain_bin:$PATH"

    log "SDK: $BL_SDK_BASE"
    log "Toolchain: $("$toolchain_bin/riscv64-unknown-elf-gcc" --version | head -1)"
    log "Target: CHIP=$CHIP BOARD=$BOARD CPU_ID=${CPU_ID:-<empty>} PROFILE=$PROFILE BUILD_DIR=$BUILD_DIR_NAME"

    cd "$PROJECT_DIR"

    local make_args=(
        "CHIP=$CHIP"
        "BOARD=$BOARD"
        "CROSS_COMPILE=$toolchain_bin/riscv64-unknown-elf-"
        "BUILD_DIR=$BUILD_DIR_NAME"
    )

    if [[ -n "$CPU_ID" ]]; then
        make_args+=("CPU_ID=$CPU_ID")
    fi
    if [[ "$PROFILE" == "dual-chip-left-spi" ]]; then
        make_args+=(
            "CONFIG_M61_DS5_DUAL_CHIP_TRANSPORT=y"
            "CONFIG_M61_ESP32_SPI_ENABLE=y"
            "CONFIG_M61_ESP32_SPI_READY=y"
            "CONFIG_M61_ESP32_SPI_SCLK_PIN=13"
            "CONFIG_M61_ESP32_SPI_MOSI_PIN=11"
            "CONFIG_M61_ESP32_SPI_MISO_PIN=10"
            "CONFIG_M61_ESP32_SPI_CS_PIN=20"
            "CONFIG_M61_ESP32_SPI_FREQ_HZ=1000000"
            "CONFIG_M61_ESP32_READY_PIN=16"
            "CONFIG_M61_ESP32_IRQ_PIN=17"
            "CONFIG_M61_ESP32_RESET_PIN=255"
            "CONFIG_M61_ESP32_RX_POLL_ENABLE=y"
            "CONFIG_M61_ESP32_RX_POLL_INTERVAL_MS=1"
            "CONFIG_M61_ESP32_ACK_POLL_COUNT=8"
            "CONFIG_M61_ESP32_RELIABLE_RETRY_COUNT=1"
            "CONFIG_M61_ESP32_TIME_SYNC_INTERVAL_MS=1000"
            "CONFIG_M61_ESP32_RESET_PULSE_MS=50"
            "CONFIG_M61_ESP32_RESET_BOOT_MS=750"
            "CONFIG_M61_ESP32_RECOVERY_ERROR_THRESHOLD=8"
            "CONFIG_M61_ESP32_RECOVERY_COOLDOWN_MS=5000"
        )
    fi

    make "${make_args[@]}"

    local bin_file
    bin_file="$(find "$PROJECT_DIR/$BUILD_DIR_NAME/build_out" -maxdepth 1 -name 'm61_dualsense_hidp_probe_*.bin' | head -1 || true)"
    if [[ -n "$bin_file" ]]; then
        log "Firmware: $bin_file"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        build|clean|all)
            COMMAND="$1"
            shift
            ;;
        --chip)
            CHIP="${2:?missing value for --chip}"
            shift 2
            ;;
        --board)
            BOARD="${2:?missing value for --board}"
            shift 2
            ;;
        --cpu-id)
            CPU_ID="${2:?missing value for --cpu-id}"
            shift 2
            ;;
        --profile)
            PROFILE="${2:?missing value for --profile}"
            case "$PROFILE" in
                none)
                    BUILD_DIR_NAME="build"
                    ;;
                dual-chip-left-spi)
                    BUILD_DIR_NAME="build_dual_chip_left_spi"
                    ;;
                *)
                    fail "unknown profile: $PROFILE"
                    ;;
            esac
            shift 2
            ;;
        -h|--help|help)
            show_help
            exit 0
            ;;
        *)
            fail "unknown argument: $1"
            ;;
    esac
done

case "$COMMAND" in
    clean)
        clean_build
        ;;
    all)
        clean_build
        build_project
        ;;
    build)
        build_project
        ;;
esac
