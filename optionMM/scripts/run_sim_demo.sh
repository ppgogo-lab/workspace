#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build-wsl}"
CONFIG_PATH="${1:-$ROOT_DIR/config/sim_user_book_demo.yaml}"

export LD_LIBRARY_PATH="$ROOT_DIR/third_party/compat_ssl/usr/lib64:$ROOT_DIR/third_party/compat_ssl:$ROOT_DIR/third_party/femas:${LD_LIBRARY_PATH:-}"

exec "$BUILD_DIR/optionmm" "$CONFIG_PATH"
