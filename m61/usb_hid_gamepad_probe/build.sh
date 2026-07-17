#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK_PATH="${BL_SDK_BASE:-}"

if [[ -z "$SDK_PATH" ]]; then
    SDK_PATH="$(cd "$PROJECT_DIR/../../../bl_mcu_sdk" && pwd)"
fi

CHIP="bl616"
BOARD="bl616dk"
COMMAND="build"

find_toolchain_bin() {
    local candidates=(
        "${M61_TOOLCHAIN_BIN:-}"
        "$HOME/riscv-toolchain/toolchain_gcc_t-head_linux/bin"
        "/opt/toolchain_gcc_t-head_linux/bin"
    )

    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -n "$candidate" && -x "$candidate/riscv64-unknown-elf-gcc" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

clean_build() {
    local build_real project_real
    project_real="$(realpath "$PROJECT_DIR")"
    if [[ -d "$PROJECT_DIR/build" ]]; then
        build_real="$(realpath "$PROJECT_DIR/build")"
        case "$build_real" in
            "$project_real"/build) rm -rf "$build_real" ;;
            *) echo "refusing to remove unexpected build path: $build_real" >&2; exit 1 ;;
        esac
    fi
}

build_project() {
    local toolchain_bin
    toolchain_bin="$(find_toolchain_bin)" || {
        echo "T-HEAD riscv64-unknown-elf-gcc not found" >&2
        exit 1
    }

    export BL_SDK_BASE="$SDK_PATH"
    export PATH="$toolchain_bin:$PATH"

    cd "$PROJECT_DIR"
    make "CHIP=$CHIP" "BOARD=$BOARD" "CROSS_COMPILE=$toolchain_bin/riscv64-unknown-elf-"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        build|clean|all) COMMAND="$1"; shift ;;
        --chip) CHIP="${2:?missing value for --chip}"; shift 2 ;;
        --board) BOARD="${2:?missing value for --board}"; shift 2 ;;
        *) echo "unknown argument: $1" >&2; exit 1 ;;
    esac
done

case "$COMMAND" in
    clean) clean_build ;;
    all) clean_build; build_project ;;
    build) build_project ;;
esac
