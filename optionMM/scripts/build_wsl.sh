#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-wsl}"
REQUESTED_BUILD_TYPE="${BUILD_TYPE:-}"
FORCE_CONFIGURE=0
BUILD_ALL=0
TARGETS=()

default_jobs() {
  if command -v nproc >/dev/null 2>&1; then
    nproc
  else
    printf '1\n'
  fi
}

JOBS="${JOBS:-$(default_jobs)}"

usage() {
  cat <<EOF
Usage: bash scripts/build_wsl.sh [options] [target...]

Build the WSL/Linux CMake tree in build-wsl.

Options:
  --configure              Force CMake configure before building
  --debug                  Configure with CMAKE_BUILD_TYPE=Debug
  --release                Configure with CMAKE_BUILD_TYPE=Release
  --relwithdebinfo         Configure with CMAKE_BUILD_TYPE=RelWithDebInfo
  --build-type <type>      Configure with an explicit CMAKE_BUILD_TYPE
  --build-dir <path>       Override the build directory (default: build-wsl)
  -j, --jobs <count>       Override build parallelism
  --all                    Build the default all target instead of optionmm
  -h, --help               Show this help

Examples:
  bash scripts/build_wsl.sh
  bash scripts/build_wsl.sh test_latency
  bash scripts/build_wsl.sh --release --configure
  bash scripts/build_wsl.sh --all
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --configure)
      FORCE_CONFIGURE=1
      shift
      ;;
    --debug)
      REQUESTED_BUILD_TYPE="Debug"
      shift
      ;;
    --release)
      REQUESTED_BUILD_TYPE="Release"
      shift
      ;;
    --relwithdebinfo)
      REQUESTED_BUILD_TYPE="RelWithDebInfo"
      shift
      ;;
    --build-type)
      REQUESTED_BUILD_TYPE="${2:?missing value for --build-type}"
      shift 2
      ;;
    --build-dir)
      BUILD_DIR="${2:?missing value for --build-dir}"
      shift 2
      ;;
    -j|--jobs)
      JOBS="${2:?missing value for $1}"
      shift 2
      ;;
    --all)
      BUILD_ALL=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      TARGETS+=("$1")
      shift
      ;;
  esac
done

case "$BUILD_DIR" in
  /*) ;;
  *) BUILD_DIR="$ROOT_DIR/$BUILD_DIR" ;;
esac

CACHE_FILE="$BUILD_DIR/CMakeCache.txt"
CONFIGURED_BUILD_TYPE=""

if [[ -f "$CACHE_FILE" ]]; then
  CONFIGURED_BUILD_TYPE="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$CACHE_FILE" | head -n 1)"
fi

if [[ ! -f "$CACHE_FILE" ]]; then
  FORCE_CONFIGURE=1
  REQUESTED_BUILD_TYPE="${REQUESTED_BUILD_TYPE:-Debug}"
elif [[ -n "$REQUESTED_BUILD_TYPE" && "$REQUESTED_BUILD_TYPE" != "$CONFIGURED_BUILD_TYPE" ]]; then
  FORCE_CONFIGURE=1
fi

if [[ $FORCE_CONFIGURE -eq 1 ]]; then
  BUILD_TYPE_TO_USE="${REQUESTED_BUILD_TYPE:-${CONFIGURED_BUILD_TYPE:-Debug}}"
  echo "Configuring $BUILD_DIR (CMAKE_BUILD_TYPE=$BUILD_TYPE_TO_USE)"
  cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE_TO_USE"
fi

BUILD_CMD=(cmake --build "$BUILD_DIR" -j "$JOBS")

if [[ $BUILD_ALL -eq 0 ]]; then
  if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=(optionmm)
  fi
  BUILD_CMD+=(--target "${TARGETS[@]}")
fi

if [[ $BUILD_ALL -eq 1 ]]; then
  echo "Building all targets in $BUILD_DIR with $JOBS job(s)"
else
  echo "Building target(s) in $BUILD_DIR: ${TARGETS[*]}"
fi

"${BUILD_CMD[@]}"
