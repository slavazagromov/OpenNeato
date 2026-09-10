#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/openneato-nogo-tests.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
    -I "$ROOT_DIR/firmware/src" \
    "$ROOT_DIR/firmware/test_host/nogo_geometry_selftest.cpp" \
    -o "$BUILD_DIR/nogo_geometry_selftest"

"$BUILD_DIR/nogo_geometry_selftest"

"${CXX:-g++}" -std=c++17 -Wall -Wextra -Werror \
    -DNOGO_CONFIG_MAX_BYTES=8192 \
    -DNOGO_MAX_LINES=16 \
    -DNOGO_MAX_POINTS_PER_LINE=32 \
    -I "$ROOT_DIR/firmware/test_host/arduino_shim" \
    -I "$ROOT_DIR/firmware/src" \
    "$ROOT_DIR/firmware/test_host/nogo_config_parser_selftest.cpp" \
    "$ROOT_DIR/firmware/src/nogo_config_parser.cpp" \
    -o "$BUILD_DIR/nogo_config_parser_selftest"

"$BUILD_DIR/nogo_config_parser_selftest"
