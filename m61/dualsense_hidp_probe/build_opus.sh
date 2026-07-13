#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="1.2.1"
ARCHIVE_SHA256="cfafd339ccd9c5ef8d6ab15d7e1a412c054bf4cb4ecbbbcc78c12ef2def70732"
ARCHIVE_URL="https://archive.mozilla.org/pub/opus/opus-${VERSION}.tar.gz"
VARIANT="${1:-O2-LTO}"
TOOLCHAIN_BIN="${2:-${M61_TOOLCHAIN_BIN:-}}"

case "$VARIANT" in
    O2)
        OPT="O2"
        LTO_FLAGS=""
        AR_NAME="riscv64-unknown-elf-ar"
        RANLIB_NAME="riscv64-unknown-elf-ranlib"
        ;;
    O2-LTO)
        OPT="O2"
        LTO_FLAGS="-flto"
        AR_NAME="riscv64-unknown-elf-gcc-ar"
        RANLIB_NAME="riscv64-unknown-elf-gcc-ranlib"
        ;;
    O3)
        OPT="O3"
        LTO_FLAGS=""
        AR_NAME="riscv64-unknown-elf-ar"
        RANLIB_NAME="riscv64-unknown-elf-ranlib"
        ;;
    *)
        printf '[m61-opus-build] ERROR: variant must be O2, O2-LTO, or O3\n' >&2
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
PATCH_FILES=(
    "$PROJECT_DIR/patches/opus-${VERSION}-disabled-prefilter-fastpath.patch"
    "$PROJECT_DIR/patches/opus-${VERSION}-e907-clz32.patch"
    "$PROJECT_DIR/patches/opus-${VERSION}-e907-exact-rcp.patch"
    "$PROJECT_DIR/patches/opus-${VERSION}-e907-q16-smmwb.patch"
    "$PROJECT_DIR/patches/opus-${VERSION}-e907-q15-kmmwb2.patch"
)
BUILD="$ROOT/build-${VARIANT}"
STAMP="$BUILD/.m61-config"
PATCH_SHA256="$(sha256sum "${PATCH_FILES[@]}" | sha256sum | awk '{print $1}')"
SOURCE_STAMP="$SOURCE/.m61-source-patch"
EXPECTED_SOURCE_STAMP="patch=${PATCH_SHA256}"
EXPECTED_STAMP="opus=${VERSION};variant=${VARIANT};patch=${PATCH_SHA256};toolchain=$($TOOLCHAIN_BIN/riscv64-unknown-elf-gcc -dumpfullversion)"

mkdir -p "$ROOT"
if [[ ! -f "$ARCHIVE" ]]; then
    printf '[m61-opus-build] Downloading %s\n' "$ARCHIVE_URL" >&2
    curl -L --fail --retry 3 "$ARCHIVE_URL" -o "$ARCHIVE"
fi

printf '%s  %s\n' "$ARCHIVE_SHA256" "$ARCHIVE" | sha256sum --check --status || {
    printf '[m61-opus-build] ERROR: archive SHA256 mismatch\n' >&2
    exit 1
}

if [[ ! -f "$SOURCE_STAMP" || "$(cat "$SOURCE_STAMP" 2>/dev/null || true)" != "$EXPECTED_SOURCE_STAMP" ]]; then
    if [[ -d "$SOURCE" ]]; then
        source_real="$(realpath "$SOURCE")"
        root_real="$(realpath "$ROOT")"
        [[ "$source_real" == "$root_real/opus-${VERSION}" ]] || {
            printf '[m61-opus-build] ERROR: refusing to remove %s\n' "$source_real" >&2
            exit 1
        }
        rm -rf "$source_real"
    fi
    tar -xzf "$ARCHIVE" -C "$ROOT"
    for patch_file in "${PATCH_FILES[@]}"; do
        patch -d "$SOURCE" -p1 < "$patch_file" >&2
    done
    printf '%s\n' "$EXPECTED_SOURCE_STAMP" > "$SOURCE_STAMP"
fi

if [[ ! -f "$STAMP" || "$(cat "$STAMP")" != "$EXPECTED_STAMP" ]]; then
    if [[ -d "$BUILD" ]]; then
        build_real="$(realpath "$BUILD")"
        root_real="$(realpath "$ROOT")"
        case "$build_real" in
            "$root_real"/build-O2|"$root_real"/build-O2-LTO|"$root_real"/build-O3) rm -rf "$build_real" ;;
            *)
                printf '[m61-opus-build] ERROR: refusing to remove %s\n' "$build_real" >&2
                exit 1
                ;;
        esac
    fi
    mkdir -p "$BUILD"
    cd "$BUILD"
    CFLAGS="-${OPT} ${LTO_FLAGS} -DM61_OPUS_E907_CLZ32=1 -DM61_OPUS_E907_EXACT_RCP=1 -DM61_OPUS_E907_Q16_SMMWB=1 -DM61_OPUS_E907_Q15_KMMWB2=1 -g0 -ffunction-sections -fdata-sections -fno-common -fstrict-volatile-bitfields -march=rv32imafcp_zpn_zpsfoperand_xtheade -mabi=ilp32f -mtune=e907" \
        "$SOURCE/configure" \
        --host=riscv64-unknown-elf \
        --disable-shared \
        --enable-static \
        --enable-fixed-point \
        --disable-doc \
        --disable-extra-programs \
        CC="$TOOLCHAIN_BIN/riscv64-unknown-elf-gcc" \
        AR="$TOOLCHAIN_BIN/$AR_NAME" \
        RANLIB="$TOOLCHAIN_BIN/$RANLIB_NAME" >&2
    printf '%s\n' "$EXPECTED_STAMP" > "$STAMP"
fi

printf '[m61-opus-build] Building Opus %s %s\n' "$VERSION" "$VARIANT" >&2
make -C "$BUILD" -j"${M61_BUILD_JOBS:-8}" >&2
LIBRARY="$BUILD/.libs/libopus.a"
[[ -f "$LIBRARY" ]] || {
    printf '[m61-opus-build] ERROR: library was not generated\n' >&2
    exit 1
}

printf '%s\n' "$LIBRARY"
