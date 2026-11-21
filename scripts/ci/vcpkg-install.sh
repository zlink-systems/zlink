#!/usr/bin/env bash
set -euo pipefail

: "${RUNNER_NAME:?RUNNER_NAME is required}"
work_root="${HOME}/.cache/zlink/vcpkg-work/${RUNNER_NAME}"
export VCPKG_DEFAULT_BINARY_CACHE="${HOME}/.cache/zlink/vcpkg-binary"
mkdir -p "${work_root}/buildtrees" "${work_root}/packages" "${work_root}/downloads" "${VCPKG_DEFAULT_BINARY_CACHE}"
if [[ -n "${VCPKG_BINARY_SOURCES:-}" ]]; then
  export VCPKG_BINARY_SOURCES="${VCPKG_BINARY_SOURCES};files,${VCPKG_DEFAULT_BINARY_CACHE},readwrite"
fi
exec "${VCPKG_ROOT}/vcpkg" install \
  "--x-buildtrees-root=${work_root}/buildtrees" \
  "--x-packages-root=${work_root}/packages" \
  "--downloads-root=${work_root}/downloads" "$@"
