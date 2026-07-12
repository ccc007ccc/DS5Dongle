#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="1.2.1"
ARCHIVE_SHA256="cfafd339ccd9c5ef8d6ab15d7e1a412c054bf4cb4ecbbbcc78c12ef2def70732"
ARCHIVE_URL="https://archive.mozilla.org/pub/opus/opus-${VERSION}.tar.gz"
OPT="${1:-O2}"
TOOLCHAIN_BIN="${2:-${M61_TOOLCHAIN_BIN:-}}"

case "$OPT" in
    O2|O3) ;;
    *)
        printf '[m61-opus-build] ERROR: optimization must be O2 or O3\n' >&2
        exit 1
        ;;
esac

if [[ -z "$TOOLCHAIN_BIN" || ! -x "$TOOLCHAIN_BIN/riscv64-unknown-elf-gcc" ]]; then
    printf '[m61-opus-build] ERROR: Xuantie toolchain bin directory is invalid: %s\n' \
        "$TOOLCHAIN_BIN" >&2
    exit 1
fi

ROOT="$PROJECT_DIR/.cache/third_party/opus-${VERSION}"
ARCHIVE="$ROOT/opus-${VERSION}.tar.gz"
SOURCE="$ROOT/opus-${VERSION}"
BUILD="$ROOT/build-${OPT}"
STAMP="$BUILD/.m61-config"
EXPECTED_STAMP="opus=${VERSION};opt=${OPT};toolchain=$($TOOLCHAIN_BIN/riscv64-unknown-elf-gcc -dumpfullversion)"

mkdir -p "$ROOT"
if [[ ! -f "$ARCHIVE" ]]; then
    printf '[m61-opus-build] Downloading %s\n' "$ARCHIVE_URL" >&2
    curl -L --fail --retry 3 "$ARCHIVE_URL" -o "$ARCHIVE"
fi

printf '%s  %s\n' "$ARCHIVE_SHA256" "$ARCHIVE" | sha256sum --check --status || {
    printf '[m61-opus-build] ERROR: archive SHA256 mismatch\n' >&2
    exit 1
}

if [[ ! -d "$SOURCE" ]]; then
    tar -xzf "$ARCHIVE" -C "$ROOT"
fi

if [[ ! -f "$STAMP" || "$(cat "$STAMP")" != "$EXPECTED_STAMP" ]]; then
    if [[ -d "$BUILD" ]]; then
        build_real="$(realpath "$BUILD")"
        root_real="$(realpath "$ROOT")"
        case "$build_real" in
            "$root_real"/build-O2|"$root_real"/build-O3) rm -rf "$build_real" ;;
            *)
                printf '[m61-opus-build] ERROR: refusing to remove %s\n' "$build_real" >&2
                exit 1
                ;;
        esac
    fi
    mkdir -p "$BUILD"
    cd "$BUILD"
    CFLAGS="-${OPT} -g0 -ffunction-sections -fdata-sections -fno-common -fstrict-volatile-bitfields -march=rv32imafcp_zpn_zpsfoperand_xtheade -mabi=ilp32f -mtune=e907" \
        "$SOURCE/configure" \
        --host=riscv64-unknown-elf \
        --disable-shared \
        --enable-static \
        --enable-fixed-point \
        --disable-doc \
        --disable-extra-programs \
        CC="$TOOLCHAIN_BIN/riscv64-unknown-elf-gcc" \
        AR="$TOOLCHAIN_BIN/riscv64-unknown-elf-ar" \
        RANLIB="$TOOLCHAIN_BIN/riscv64-unknown-elf-ranlib" >&2
    printf '%s\n' "$EXPECTED_STAMP" > "$STAMP"
fi

printf '[m61-opus-build] Building Opus %s -%s\n' "$VERSION" "$OPT" >&2
make -C "$BUILD" -j"${M61_BUILD_JOBS:-8}" >&2
LIBRARY="$BUILD/.libs/libopus.a"
[[ -f "$LIBRARY" ]] || {
    printf '[m61-opus-build] ERROR: library was not generated\n' >&2
    exit 1
}

printf '%s\n' "$LIBRARY"
