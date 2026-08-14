#!/usr/bin/env bash

ZLINK_BINDINGS_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZLINK_BINDINGS_ROOT="$(cd "${ZLINK_BINDINGS_TOOLS_DIR}/.." && pwd)"
ZLINK_REPO_ROOT="$(cd "${ZLINK_BINDINGS_ROOT}/.." && pwd)"
ZLINK_VERSION_FILE="${ZLINK_REPO_ROOT}/VERSION"
ZLINK_CORE_SOURCE="${ZLINK_CORE_SOURCE:-release}"
ZLINK_REPOSITORY_VERSION="$(awk -F= '/^LIBZLINK_VERSION=/{print $2}' "${ZLINK_VERSION_FILE}")"
ZLINK_CORE_VERSION="${ZLINK_CORE_RELEASE_VERSION:-$ZLINK_REPOSITORY_VERSION}"
if [[ "$ZLINK_CORE_VERSION" != "$ZLINK_REPOSITORY_VERSION" ]]; then
  echo "ZLINK_CORE_RELEASE_VERSION $ZLINK_CORE_VERSION must match repository VERSION $ZLINK_REPOSITORY_VERSION" >&2
  return 2 2>/dev/null || exit 2
fi
ZLINK_CORE_MAJOR="${ZLINK_CORE_VERSION%%.*}"
ZLINK_CORE_RELEASE_MODE=0
ZLINK_CORE_PACKAGE_PREFIX="${ZLINK_CORE_PACKAGE_PREFIX:-}"

case "${ZLINK_CORE_SOURCE}" in
  release)
    if [[ -z "${ZLINK_CORE_PACKAGE_PREFIX}" ]]; then
      release_args=(--version "${ZLINK_CORE_VERSION}")
      if [[ -n "${ZLINK_CORE_CACHE_DIR:-}" ]]; then
        release_args+=(--cache-dir "${ZLINK_CORE_CACHE_DIR}")
      fi
      ZLINK_CORE_PACKAGE_PREFIX="$(bash "${ZLINK_REPO_ROOT}/scripts/local-package/core/fetch-release.sh" "${release_args[@]}")"
    fi
    [[ -d "${ZLINK_CORE_PACKAGE_PREFIX}" ]] || {
      echo "Core release prefix not found: ${ZLINK_CORE_PACKAGE_PREFIX}" >&2
      return 1 2>/dev/null || exit 1
    }
    core_manifest="${ZLINK_CORE_PACKAGE_PREFIX}/share/zlink/core-package-provenance.json"
    [[ -f "${core_manifest}" ]] || {
      echo "Core release provenance not found: ${core_manifest}" >&2
      return 1 2>/dev/null || exit 1
    }
    core_manifest_version="$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "${core_manifest}" | head -n1)"
    [[ "${core_manifest_version}" == "${ZLINK_CORE_VERSION}" ]] || {
      echo "Core release prefix version ${core_manifest_version} does not match ${ZLINK_CORE_VERSION}" >&2
      return 1 2>/dev/null || exit 1
    }
    ZLINK_CORE_RELEASE_MODE=1
    ;;
  local)
    ;;
  *)
    echo "ZLINK_CORE_SOURCE must be release or local: ${ZLINK_CORE_SOURCE}" >&2
    return 1 2>/dev/null || exit 1
    ;;
esac

if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
  ZLINK_CORE_LIB_DIR="${ZLINK_CORE_PACKAGE_PREFIX}/lib"
  ZLINK_CORE_INCLUDE_DIR="${ZLINK_CORE_PACKAGE_PREFIX}/include"
else
  ZLINK_CORE_LIB_DIR="${ZLINK_REPO_ROOT}/core/build/lib"
  ZLINK_CORE_INCLUDE_DIR="${ZLINK_REPO_ROOT}/core/include"
fi
export ZLINK_CORE_PACKAGE_PREFIX ZLINK_CORE_RELEASE_MODE ZLINK_CORE_SOURCE
export ZLINK_CORE_VERSION ZLINK_CORE_LIB_DIR ZLINK_CORE_INCLUDE_DIR
ZLINK_IS_WINDOWS_NATIVE=0
case "$(uname -s 2>/dev/null || true)" in
  MINGW*|MSYS*|CYGWIN*)
    ZLINK_IS_WINDOWS_NATIVE=1
    if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
      ZLINK_LOCAL_CORE_RUNTIME="${ZLINK_CORE_PACKAGE_PREFIX}/bin/zlink.dll"
    else
      ZLINK_LOCAL_CORE_RUNTIME="${ZLINK_REPO_ROOT}/core/build/windows-x64/install/bin/zlink.dll"
    fi
    ;;
  *)
    ZLINK_CORE_VERSIONED_LIB="${ZLINK_CORE_LIB_DIR}/libzlink.so.${ZLINK_CORE_VERSION}"
    ZLINK_CORE_UNVERSIONED_LIB="${ZLINK_CORE_LIB_DIR}/libzlink.so"
    if [[ "$(uname -s 2>/dev/null || true)" == Darwin* ]]; then
      ZLINK_CORE_VERSIONED_LIB="${ZLINK_CORE_LIB_DIR}/libzlink.dylib"
      ZLINK_CORE_UNVERSIONED_LIB="${ZLINK_CORE_VERSIONED_LIB}"
    fi
    ZLINK_LOCAL_CORE_RUNTIME="${ZLINK_CORE_VERSIONED_LIB}"

    if [[ ! -f "${ZLINK_LOCAL_CORE_RUNTIME}" && -f "${ZLINK_CORE_UNVERSIONED_LIB}" ]]; then
      ZLINK_LOCAL_CORE_RUNTIME="${ZLINK_CORE_UNVERSIONED_LIB}"
    fi
    ;;
esac

zlink_has_local_core_runtime() {
  [[ -f "${ZLINK_LOCAL_CORE_RUNTIME}" ]]
}

zlink_export_local_core_runtime() {
  if zlink_has_local_core_runtime; then
    export ZLINK_LIBRARY_PATH="${ZLINK_LOCAL_CORE_RUNTIME}"
  fi
}

zlink_sync_linux_native_dir() {
  [[ "${ZLINK_IS_WINDOWS_NATIVE}" -eq 0 ]] || return 0
  local native_dir="$1"
  [[ -d "${native_dir}" ]] || return 0
  zlink_has_local_core_runtime || return 0

  rm -f "${native_dir}/libzlink.so" \
    "${native_dir}/libzlink.so.${ZLINK_CORE_MAJOR}" \
    "${native_dir}/libzlink.so."*
  cp -f "${ZLINK_LOCAL_CORE_RUNTIME}" "${native_dir}/libzlink.so.${ZLINK_CORE_VERSION}"
  ln -sfn "libzlink.so.${ZLINK_CORE_VERSION}" "${native_dir}/libzlink.so.${ZLINK_CORE_MAJOR}"
  ln -sfn "libzlink.so.${ZLINK_CORE_MAJOR}" "${native_dir}/libzlink.so"
}

zlink_sync_linux_native_dirs_by_find() {
  [[ "${ZLINK_IS_WINDOWS_NATIVE}" -eq 0 ]] || return 0
  local search_root="$1"
  local native_path_pattern="${2:-*linux-x64/native}"
  [[ -d "${search_root}" ]] || return 0
  zlink_has_local_core_runtime || return 0

  while IFS= read -r native_dir; do
    zlink_sync_linux_native_dir "${native_dir}"
  done < <(find "${search_root}" -type d -path "${native_path_pattern}")
}
