#!/usr/bin/env bash
set -euo pipefail

PICO_SDK_VERSION="2.2.0"
TINYUSB_VERSION="0.20.0"
BUILD_TYPE="Release"
BUILD_DIR="build/wake"
ENABLE_WAKE_HID="ON"
SDK_DIR=""
DEFAULT_SDK_DIR=""
USING_DEFAULT_SDK="ON"
CLEAN_BUILD="OFF"

usage() {
  cat <<'USAGE'
Usage: tools/build-macos.sh [options]

Build DS5Dongle on macOS using a repo-local Pico SDK checkout.

Options:
  --standard          Build standard firmware without ENABLE_WAKE_HID.
  --wake              Build wake firmware with ENABLE_WAKE_HID (default).
  --clean             Remove the selected build directory before configuring.
  --sdk-dir <path>    Use this Pico SDK checkout instead of .deps/pico-sdk.
  -h, --help          Show this help.

The script prompts before installing missing Homebrew packages.
USAGE
}

die() {
  echo "error: $*" >&2
  exit 1
}

confirm() {
  local prompt="$1"
  local answer
  read -r -p "$prompt [y/N] " answer
  case "$answer" in
    y|Y|yes|YES) return 0 ;;
    *) return 1 ;;
  esac
}

repo_root() {
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
  cd "$script_dir/.." && pwd -P
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --standard)
      ENABLE_WAKE_HID="OFF"
      BUILD_DIR="build/standard"
      shift
      ;;
    --wake)
      ENABLE_WAKE_HID="ON"
      BUILD_DIR="build/wake"
      shift
      ;;
    --clean)
      CLEAN_BUILD="ON"
      shift
      ;;
    --sdk-dir)
      [[ $# -ge 2 ]] || die "--sdk-dir requires a path"
      SDK_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

if [[ "$(uname -s)" != "Darwin" ]]; then
  die "this script is intended for macOS"
fi

ROOT="$(repo_root)"
cd "$ROOT"

DEFAULT_SDK_DIR="$ROOT/.deps/pico-sdk"
if [[ -z "$SDK_DIR" ]]; then
  SDK_DIR="$DEFAULT_SDK_DIR"
else
  USING_DEFAULT_SDK="OFF"
fi
SDK_PARENT="$(cd "$(dirname "$SDK_DIR")" 2>/dev/null && pwd -P || true)"
if [[ -n "$SDK_PARENT" ]]; then
  SDK_DIR="$SDK_PARENT/$(basename "$SDK_DIR")"
fi

echo "Repository: $ROOT"
echo "Pico SDK:   $SDK_DIR"
echo "Build dir:  $BUILD_DIR"
echo "Wake HID:   $ENABLE_WAKE_HID"
echo

if ! xcode-select -p >/dev/null 2>&1; then
  echo "Xcode Command Line Tools are required."
  if confirm "Open Apple's installer with xcode-select --install?"; then
    xcode-select --install
    echo "Re-run this script after the installer finishes."
    exit 1
  fi
  die "Xcode Command Line Tools are missing"
fi

command -v git >/dev/null 2>&1 || die "git is required"

missing_brew_packages=()
command -v cmake >/dev/null 2>&1 || missing_brew_packages+=("cmake")
command -v ninja >/dev/null 2>&1 || missing_brew_packages+=("ninja")
command -v arm-none-eabi-gcc >/dev/null 2>&1 || missing_brew_packages+=("arm-none-eabi-gcc")

if [[ ${#missing_brew_packages[@]} -gt 0 ]]; then
  command -v brew >/dev/null 2>&1 || die "Homebrew is required to install: ${missing_brew_packages[*]}"
  echo "Missing build dependencies: ${missing_brew_packages[*]}"
  if confirm "Install missing packages with Homebrew?"; then
    brew install "${missing_brew_packages[@]}"
  else
    die "missing required build dependencies"
  fi
fi

echo "Initializing repository submodules..."
git submodule update --init --recursive

if [[ -e "$SDK_DIR" ]] && ! git -C "$SDK_DIR" rev-parse --git-dir >/dev/null 2>&1; then
  die "$SDK_DIR exists but is not a Git checkout"
fi

if [[ -e "$SDK_DIR" ]] && [[ "$USING_DEFAULT_SDK" == "OFF" ]]; then
  echo "The SDK at $SDK_DIR will be checked out to Pico SDK $PICO_SDK_VERSION,"
  echo "and its TinyUSB submodule will be checked out to $TINYUSB_VERSION."
  if ! confirm "Continue modifying this SDK checkout?"; then
    die "refusing to modify user-provided SDK checkout"
  fi
fi

if [[ ! -e "$SDK_DIR" ]]; then
  mkdir -p "$(dirname "$SDK_DIR")"
  echo "Cloning Pico SDK $PICO_SDK_VERSION..."
  git clone https://github.com/raspberrypi/pico-sdk.git "$SDK_DIR"
fi

echo "Preparing Pico SDK $PICO_SDK_VERSION..."
git -C "$SDK_DIR" fetch --tags
git -C "$SDK_DIR" checkout "$PICO_SDK_VERSION"
git -C "$SDK_DIR" submodule update --init --recursive

TINYUSB_DIR="$SDK_DIR/lib/tinyusb"
git -C "$TINYUSB_DIR" rev-parse --git-dir >/dev/null 2>&1 || die "TinyUSB submodule not found at $TINYUSB_DIR"

echo "Preparing TinyUSB $TINYUSB_VERSION..."
git -C "$TINYUSB_DIR" fetch --tags
git -C "$TINYUSB_DIR" checkout "$TINYUSB_VERSION"
git -C "$TINYUSB_DIR" submodule update --init --recursive

if [[ "$CLEAN_BUILD" == "ON" ]]; then
  echo "Removing $BUILD_DIR..."
  rm -rf "$BUILD_DIR"
fi

echo "Configuring firmware..."
cmake -S . -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DPICO_SDK_PATH="$SDK_DIR" \
  -DENABLE_WAKE_HID="$ENABLE_WAKE_HID"

echo "Building firmware..."
cmake --build "$BUILD_DIR" --target ds5-bridge

echo
echo "Built firmware:"
echo "  $ROOT/$BUILD_DIR/ds5-bridge.uf2"
