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

log() {
    printf '[m61-hidp-build] %s\n' "$*"
}

fail() {
    printf '[m61-hidp-build] ERROR: %s\n' "$*" >&2
    exit 1
}

show_help() {
    cat <<'EOF'
Usage: ./build.sh [build|clean|all] [--chip bl616] [--board bl616dk] [--cpu-id ap]

Builds the M61 DualSense Classic Bluetooth HIDP probe.

Environment:
  BL_SDK_BASE       Optional Bouffalo SDK path.
  M61_TOOLCHAIN_BIN Optional T-HEAD toolchain bin directory.

Example:
  ./build.sh
  ./build.sh all
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

    local toolchain_bin
    toolchain_bin="$(find_toolchain_bin)" || fail "T-HEAD riscv64-unknown-elf-gcc not found"

    export BL_SDK_BASE="$SDK_PATH"
    export PATH="$toolchain_bin:$PATH"

    log "SDK: $BL_SDK_BASE"
    log "Toolchain: $("$toolchain_bin/riscv64-unknown-elf-gcc" --version | head -1)"
    log "Target: CHIP=$CHIP BOARD=$BOARD CPU_ID=${CPU_ID:-<empty>}"

    cd "$PROJECT_DIR"

    local make_args=(
        "CHIP=$CHIP"
        "BOARD=$BOARD"
        "CROSS_COMPILE=$toolchain_bin/riscv64-unknown-elf-"
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
