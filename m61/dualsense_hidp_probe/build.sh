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
HPM_PROFILE="n"
HPM_SAMPLE_SHIFT="${M61_HPM_SAMPLE_SHIFT:-4}"
USB_GAMEPAD_O2="n"
CODEC_PAIR_DELAY_MS="${M61_CODEC_PAIR_DELAY_MS:-1}"
CRC32_NIBBLE_TABLE="${M61_CRC32_NIBBLE_TABLE:-1}"
RUNTIME_PROFILE="n"
CPU_OVERCLOCK_MHZ="${M61_CPU_OVERCLOCK_MHZ:-0}"
PIPELINE_PROFILE="n"
MEMORY_BENCH="n"
OPUS_STAGE_PROFILE="n"
MIC_PROFILE="n"
OPUS_LIBRARY="${M61_OPUS_LIBRARY:-}"
OPUS_VARIANT="${M61_OPUS_VARIANT:-source-o2-lto}"
OPUS_TCM_PROFILE="${M61_OPUS_TCM_PROFILE:-pvq-mdct-decode-mdct}"

log() {
    printf '[m61-hidp-build] %s\n' "$*"
}

fail() {
    printf '[m61-hidp-build] ERROR: %s\n' "$*" >&2
    exit 1
}

show_help() {
    cat <<'EOF'
Usage: ./build.sh [build|clean|all] [--chip bl616] [--board bl616dk] [--cpu-id ap] [--hpm-profile] [--hpm-sample-shift 0..8] [--usb-gamepad-o2] [--codec-pair-delay-ms 1|2] [--crc32-nibble-table 0|1] [--runtime-profile] [--cpu-overclock 0|384|400|420|460|480] [--pipeline-profile] [--mic-profile] [--memory-bench] [--opus-stage-profile] [--opus-tcm-profile PROFILE] [--opus-sdk|--opus-source-o2|--opus-source-o2-lto|--opus-source-o3|--opus-library PATH]

Builds the M61 DualSense Classic Bluetooth HIDP probe.

Environment:
  BL_SDK_BASE       Optional Bouffalo SDK path.
  M61_TOOLCHAIN_BIN Optional T-HEAD toolchain bin directory.
  M61_OPUS_LIBRARY  Optional source-built libopus.a used instead of the SDK archive.
  M61_OPUS_VARIANT  source-o2-lto (default), source-o2, source-o3, sdk, or custom.
  M61_OPUS_TCM_PROFILE  pvq-mdct-decode-mdct (default); other placements are experimental.
  M61_OPUS_STAGE_PROFILE  y enables test-only CELT stage markers.
  M61_HPM_SAMPLE_SHIFT    HPM sampling shift, 0=all frames, 4=about 1/16.
  --usb-gamepad-o2        Compile only m61_usb_gamepad.c with -O2 (A/B).
  M61_CODEC_PAIR_DELAY_MS Codec task paired encode+decode wait window, 1 or 2 ms (default 1).
  M61_CRC32_NIBBLE_TABLE  Flash-resident 16-entry CRC table, 0 or 1 (default 1).
  --runtime-profile       Enable diagnostic FreeRTOS task runtime/idle accounting.
  M61_CPU_OVERCLOCK_MHZ   CPU target: 0 (off), 384, 400, 420, 460, or 480 MHz.

Example:
  ./build.sh
  ./build.sh all
  ./build.sh all --hpm-profile
  ./build.sh all --memory-bench
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
    if [[ -d "$PROJECT_DIR/build" ]]; then
        build_real="$(realpath "$PROJECT_DIR/build")"
        case "$build_real" in
            "$project_real"/build)
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
    [[ "$HPM_SAMPLE_SHIFT" =~ ^[0-8]$ ]] ||
        fail "HPM sample shift must be an integer from 0 to 8"
    [[ "$CODEC_PAIR_DELAY_MS" =~ ^(1|2)$ ]] ||
        fail "codec pair delay must be 1 or 2 ms"
    [[ "$CRC32_NIBBLE_TABLE" =~ ^(0|1)$ ]] ||
        fail "CRC32 nibble table must be 0 or 1"
    [[ "$CPU_OVERCLOCK_MHZ" =~ ^(0|384|400|420|460|480)$ ]] ||
        fail "CPU overclock must be 0, 384, 400, 420, 460, or 480 MHz"

    local toolchain_bin
    local opus_stage_value=0
    toolchain_bin="$(find_toolchain_bin)" || fail "T-HEAD riscv64-unknown-elf-gcc not found"
    if [[ "$OPUS_STAGE_PROFILE" == "y" ]]; then
        opus_stage_value=1
    fi

    case "$OPUS_VARIANT" in
        source-o2)
            OPUS_LIBRARY="$(M61_OPUS_TCM_PROFILE="$OPUS_TCM_PROFILE" M61_OPUS_STAGE_PROFILE="$opus_stage_value" bash "$PROJECT_DIR/build_opus.sh" O2 "$toolchain_bin")"
            ;;
        source-o2-lto)
            OPUS_LIBRARY="$(M61_OPUS_TCM_PROFILE="$OPUS_TCM_PROFILE" M61_OPUS_STAGE_PROFILE="$opus_stage_value" bash "$PROJECT_DIR/build_opus.sh" O2-LTO "$toolchain_bin")"
            ;;
        source-o3)
            OPUS_LIBRARY="$(M61_OPUS_TCM_PROFILE="$OPUS_TCM_PROFILE" M61_OPUS_STAGE_PROFILE="$opus_stage_value" bash "$PROJECT_DIR/build_opus.sh" O3 "$toolchain_bin")"
            ;;
        sdk)
            OPUS_LIBRARY=""
            ;;
        custom)
            [[ -n "$OPUS_LIBRARY" ]] || fail "custom Opus variant requires --opus-library"
            ;;
        *)
            fail "unknown Opus variant: $OPUS_VARIANT"
            ;;
    esac

    export BL_SDK_BASE="$SDK_PATH"
    export PATH="$toolchain_bin:$PATH"
    export M61_OPUS_LIBRARY="$OPUS_LIBRARY"

    log "SDK: $BL_SDK_BASE"
    log "Toolchain: $("$toolchain_bin/riscv64-unknown-elf-gcc" --version | head -1)"
    log "Target: CHIP=$CHIP BOARD=$BOARD CPU_ID=${CPU_ID:-<empty>}"
    log "Opus: ${OPUS_LIBRARY:-SDK prebuilt}"
    log "Opus TCM profile: $OPUS_TCM_PROFILE"
    log "Opus stage profile: $OPUS_STAGE_PROFILE"
    log "Mic diagnostic profile: $MIC_PROFILE"
    log "HPM sample shift: $HPM_SAMPLE_SHIFT (about 1/$((1 << HPM_SAMPLE_SHIFT)))"
    log "USB gamepad TU O2: $USB_GAMEPAD_O2"
    log "Codec pair delay: $CODEC_PAIR_DELAY_MS ms"
    log "CRC32 nibble table: $CRC32_NIBBLE_TABLE (Flash-resident)"
    log "Runtime profile: $RUNTIME_PROFILE (diagnostic only)"
    log "CPU overclock: $CPU_OVERCLOCK_MHZ MHz (0=off)"

    cd "$PROJECT_DIR"

    local make_args=(
        "CHIP=$CHIP"
        "BOARD=$BOARD"
        "CROSS_COMPILE=$toolchain_bin/riscv64-unknown-elf-"
        "CONFIG_M61_HPM_PROFILE=$HPM_PROFILE"
        "CONFIG_M61_HPM_SAMPLE_SHIFT=$HPM_SAMPLE_SHIFT"
        "CONFIG_M61_USB_GAMEPAD_O2=$USB_GAMEPAD_O2"
        "CONFIG_M61_CODEC_PAIR_DELAY_MS=$CODEC_PAIR_DELAY_MS"
        "CONFIG_M61_CRC32_NIBBLE_TABLE=$CRC32_NIBBLE_TABLE"
        "CONFIG_M61_RUNTIME_PROFILE=$RUNTIME_PROFILE"
        "CONFIG_M61_CPU_OVERCLOCK_MHZ=$CPU_OVERCLOCK_MHZ"
        "CONFIG_M61_PIPELINE_PROFILE=$PIPELINE_PROFILE"
        "CONFIG_M61_MEMORY_BENCH=$MEMORY_BENCH"
        "CONFIG_M61_OPUS_STAGE_PROFILE=$OPUS_STAGE_PROFILE"
        "CONFIG_M61_DS5_MIC_DEFAULT_ENABLED=$MIC_PROFILE"
    )

    if [[ -n "$CPU_ID" ]]; then
        make_args+=("CPU_ID=$CPU_ID")
    fi

    make "${make_args[@]}"

    local bin_file
    bin_file="$(find "$PROJECT_DIR/build/build_out" -maxdepth 1 -name 'm61_dualsense_hidp_probe_*.bin' | head -1 || true)"
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
        --hpm-profile)
            HPM_PROFILE="y"
            shift
            ;;
        --hpm-sample-shift)
            HPM_SAMPLE_SHIFT="${2:?missing value for --hpm-sample-shift}"
            [[ "$HPM_SAMPLE_SHIFT" =~ ^[0-8]$ ]] ||
                fail "--hpm-sample-shift must be an integer from 0 to 8"
            shift 2
            ;;
        --usb-gamepad-o2)
            USB_GAMEPAD_O2="y"
            shift
            ;;
        --codec-pair-delay-ms)
            CODEC_PAIR_DELAY_MS="${2:?missing value for --codec-pair-delay-ms}"
            [[ "$CODEC_PAIR_DELAY_MS" =~ ^(1|2)$ ]] ||
                fail "--codec-pair-delay-ms must be 1 or 2"
            shift 2
            ;;
        --crc32-nibble-table)
            CRC32_NIBBLE_TABLE="${2:?missing value for --crc32-nibble-table}"
            [[ "$CRC32_NIBBLE_TABLE" =~ ^(0|1)$ ]] ||
                fail "--crc32-nibble-table must be 0 or 1"
            shift 2
            ;;
        --runtime-profile)
            RUNTIME_PROFILE="y"
            shift
            ;;
        --cpu-overclock)
            CPU_OVERCLOCK_MHZ="${2:?missing value for --cpu-overclock}"
            [[ "$CPU_OVERCLOCK_MHZ" =~ ^(0|384|400|420|460|480)$ ]] ||
                fail "--cpu-overclock must be 0, 384, 400, 420, 460, or 480"
            shift 2
            ;;
        --pipeline-profile)
            HPM_PROFILE="y"
            PIPELINE_PROFILE="y"
            shift
            ;;
        --mic-profile)
            HPM_PROFILE="y"
            MIC_PROFILE="y"
            shift
            ;;
        --memory-bench)
            MEMORY_BENCH="y"
            HPM_PROFILE="y"
            shift
            ;;
        --opus-stage-profile)
            OPUS_STAGE_PROFILE="y"
            HPM_PROFILE="y"
            shift
            ;;
        --opus-tcm-profile)
            OPUS_TCM_PROFILE="${2:?missing value for --opus-tcm-profile}"
            shift 2
            ;;
        --opus-library)
            OPUS_LIBRARY="${2:?missing value for --opus-library}"
            OPUS_VARIANT="custom"
            shift 2
            ;;
        --opus-sdk)
            OPUS_VARIANT="sdk"
            shift
            ;;
        --opus-source-o2)
            OPUS_VARIANT="source-o2"
            shift
            ;;
        --opus-source-o2-lto)
            OPUS_VARIANT="source-o2-lto"
            shift
            ;;
        --opus-source-o3)
            OPUS_VARIANT="source-o3"
            shift
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
