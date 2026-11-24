#!/usr/bin/env bash
set -euo pipefail

: "${RUNNER_NAME:?RUNNER_NAME is required}"
# Hosted runner names contain spaces ("GitHub Actions 1000060908"); OpenSSL's
# configure fails under a path with spaces, so keep only path-safe characters.
runner_dir="${RUNNER_NAME//[^A-Za-z0-9._-]/_}"
cache_root="${XDG_CACHE_HOME:-${HOME}/.cache}/zlink"
work_root="${cache_root}/vcpkg-work/${runner_dir}"
export VCPKG_DEFAULT_BINARY_CACHE="${cache_root}/vcpkg-binary"
mkdir -p "${work_root}/buildtrees" "${work_root}/packages" "${work_root}/downloads" "${VCPKG_DEFAULT_BINARY_CACHE}"
if [[ -n "${VCPKG_BINARY_SOURCES:-}" ]]; then
  export VCPKG_BINARY_SOURCES="${VCPKG_BINARY_SOURCES};files,${VCPKG_DEFAULT_BINARY_CACHE},readwrite"
fi
exec "${VCPKG_ROOT}/vcpkg" install \
  "--x-buildtrees-root=${work_root}/buildtrees" \
  "--x-packages-root=${work_root}/packages" \
  "--downloads-root=${work_root}/downloads" "$@"
