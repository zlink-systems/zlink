#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
mkdir -p "${BUILD_DIR}"

CXX_BIN="${CXX:-g++}"
OUT="${BUILD_DIR}/perf_stream_client"

"${CXX_BIN}" \
  -O2 \
  -std=c++17 \
  -Wall \
  -Wextra \
  "${SCRIPT_DIR}/perf_stream_client.cpp" \
  -o "${OUT}" \
  -pthread \
  -lssl \
  -lcrypto

echo "${OUT}"
