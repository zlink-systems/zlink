#!/usr/bin/env bash
set -euo pipefail

# setup_grpc_dist.sh
# Copies gRPC + Protobuf libraries from system installation to grpc_dist/linux-x64/.
# Run this once before building gRPC benchmarks.
#
# Prerequisites:
#   - gRPC and Protobuf installed via package manager or built from source
#   - protoc and grpc_cpp_plugin available in PATH or specified via GRPC_PREFIX
#
# Usage:
#   ./setup_grpc_dist.sh                      # Auto-detect from system
#   GRPC_PREFIX=/usr/local ./setup_grpc_dist.sh  # Specify prefix

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIST_DIR="${SCRIPT_DIR}/grpc/grpc_dist/linux-x64"

GRPC_PREFIX="${GRPC_PREFIX:-/usr/local}"

echo "Setting up gRPC dist in: ${DIST_DIR}"
echo "Using GRPC_PREFIX: ${GRPC_PREFIX}"

# Create directories
mkdir -p "${DIST_DIR}/include"
mkdir -p "${DIST_DIR}/lib"
mkdir -p "${DIST_DIR}/bin"

# Copy protoc
if [[ -f "${GRPC_PREFIX}/bin/protoc" ]]; then
  cp -v "${GRPC_PREFIX}/bin/protoc" "${DIST_DIR}/bin/"
elif command -v protoc &>/dev/null; then
  cp -v "$(command -v protoc)" "${DIST_DIR}/bin/"
else
  echo "Warning: protoc not found" >&2
fi

# Copy grpc_cpp_plugin
if [[ -f "${GRPC_PREFIX}/bin/grpc_cpp_plugin" ]]; then
  cp -v "${GRPC_PREFIX}/bin/grpc_cpp_plugin" "${DIST_DIR}/bin/"
elif command -v grpc_cpp_plugin &>/dev/null; then
  cp -v "$(command -v grpc_cpp_plugin)" "${DIST_DIR}/bin/"
else
  echo "Warning: grpc_cpp_plugin not found" >&2
fi

# Copy headers
for dir in grpc grpcpp google; do
  if [[ -d "${GRPC_PREFIX}/include/${dir}" ]]; then
    cp -rv "${GRPC_PREFIX}/include/${dir}" "${DIST_DIR}/include/"
  fi
done

# Detect library directory (handles Debian/Ubuntu multiarch)
GRPC_LIB_DIR="${GRPC_PREFIX}/lib"
if [[ ! -f "${GRPC_LIB_DIR}/libgrpc++.so" ]]; then
  MULTIARCH_TRIPLET="$(dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null || true)"
  if [[ -n "${MULTIARCH_TRIPLET}" && -f "${GRPC_PREFIX}/lib/${MULTIARCH_TRIPLET}/libgrpc++.so" ]]; then
    GRPC_LIB_DIR="${GRPC_PREFIX}/lib/${MULTIARCH_TRIPLET}"
  fi
fi
echo "Using lib dir: ${GRPC_LIB_DIR}"

# Copy shared libraries
copy_libs() {
  local pattern="$1"
  for f in ${GRPC_LIB_DIR}/${pattern}; do
    if [[ -f "$f" || -L "$f" ]]; then
      cp -av "$f" "${DIST_DIR}/lib/"
    fi
  done
}

copy_libs "libgrpc++.so*"
copy_libs "libgrpc.so*"
copy_libs "libgpr.so*"
copy_libs "libprotobuf.so*"
copy_libs "libaddress_sorting.so*"
copy_libs "libupb*.so*"
copy_libs "libre2.so*"
copy_libs "libabsl_*.so*"
copy_libs "libcares.so*"

echo ""
echo "Done. Contents of ${DIST_DIR}:"
echo "  bin/:"
ls -la "${DIST_DIR}/bin/" 2>/dev/null || echo "    (empty)"
echo "  lib/:"
ls -la "${DIST_DIR}/lib/" 2>/dev/null | head -20 || echo "    (empty)"
echo "  include/:"
ls -d "${DIST_DIR}/include/"*/ 2>/dev/null || echo "    (empty)"
