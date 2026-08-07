#!/usr/bin/env bash

ZLINK_BINDINGS_TOOLS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ZLINK_BINDINGS_ROOT="$(cd "${ZLINK_BINDINGS_TOOLS_DIR}/.." && pwd)"
ZLINK_REPO_ROOT="$(cd "${ZLINK_BINDINGS_ROOT}/.." && pwd)"
ZLINK_VERSION_FILE="${ZLINK_REPO_ROOT}/VERSION"
ZLINK_CORE_LIB_DIR="${ZLINK_REPO_ROOT}/core/build/lib"
ZLINK_CORE_VERSION="$(awk -F= '/^LIBZLINK_VERSION=/{print $2}' "${ZLINK_VERSION_FILE}")"
ZLINK_CORE_MAJOR="${ZLINK_CORE_VERSION%%.*}"
ZLINK_IS_WINDOWS_NATIVE=0
case "$(uname -s 2>/dev/null || true)" in
  MINGW*|MSYS*|CYGWIN*)
    ZLINK_IS_WINDOWS_NATIVE=1
    ZLINK_LOCAL_CORE_RUNTIME="${ZLINK_REPO_ROOT}/core/build/windows-x64/install/bin/zlink.dll"
    ;;
  *)
    ZLINK_CORE_VERSIONED_LIB="${ZLINK_CORE_LIB_DIR}/libzlink.so.${ZLINK_CORE_VERSION}"
    ZLINK_CORE_UNVERSIONED_LIB="${ZLINK_CORE_LIB_DIR}/libzlink.so"
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
