#!/usr/bin/env bash

format_elapsed() {
  local total_sec="${1:-0}"
  local hours=$(( total_sec / 3600 ))
  local minutes=$(( (total_sec % 3600) / 60 ))
  local seconds=$(( total_sec % 60 ))
  if (( hours > 0 )); then
    printf "%dh %dm %ds" "${hours}" "${minutes}" "${seconds}"
  elif (( minutes > 0 )); then
    printf "%dm %ds" "${minutes}" "${seconds}"
  else
    printf "%ds" "${seconds}"
  fi
}

print_total_time() {
  if [[ "${SHOW_TOTAL_TIME}" -ne 1 ]]; then
    return
  fi
  if [[ "${PERF_SUPPRESS_TOTAL_TIME:-0}" == "1" ]]; then
    return
  fi
  local status="${1:-0}"
  local elapsed="${SECONDS}"
  echo "Total benchmark time: $(format_elapsed "${elapsed}") (${elapsed}s, exit=${status})"
}

is_uint() {
  local value="${1:-}"
  [[ "${value}" =~ ^[0-9]+$ ]]
}

resolve_configured_core_build_dir() {
  local build_dir="${1:-${OFFICIAL_BUILD_DIR}}"
  local cache_path="${build_dir}/CMakeCache.txt"
  local configured_dir=""
  if [[ "${ZLINK_CORE_RELEASE_MODE:-0}" -eq 1 ]]; then
    printf '%s\n' "${ZLINK_CORE_PACKAGE_PREFIX}"
    return
  fi
  if [[ -f "${cache_path}" ]]; then
    configured_dir="$(
      sed -n 's/^ZLINK_CPP_CORE_BUILD_DIR:PATH=//p' "${cache_path}" | tail -n 1
    )"
  fi
  if [[ -z "${configured_dir}" ]]; then
    configured_dir="${DEFAULT_CORE_BUILD_DIR}"
  fi
  realpath -m "${configured_dir}"
}

resolve_core_runtime_library() {
  local core_build_dir="${1:-}"
  local candidates=()
  case "$(uname -s)" in
    Linux*)
      candidates=(
        "${core_build_dir}/lib/libzlink.so"
        "${core_build_dir}/bin/libzlink.so"
      )
      ;;
    Darwin*)
      candidates=(
        "${core_build_dir}/lib/libzlink.dylib"
        "${core_build_dir}/bin/libzlink.dylib"
      )
      ;;
    MINGW*|MSYS*|CYGWIN*)
      candidates=(
        "${core_build_dir}/bin/zlink.dll"
        "${core_build_dir}/lib/zlink.dll"
      )
      ;;
    *)
      candidates=(
        "${core_build_dir}/lib/libzlink.so"
        "${core_build_dir}/bin/libzlink.so"
      )
      ;;
  esac

  local candidate=""
  for candidate in "${candidates[@]}"; do
    if [[ -f "${candidate}" ]]; then
      realpath -e "${candidate}"
      return 0
    fi
  done
  return 1
}

prepare_core_runtime_metadata() {
  local core_build_dir=""
  local provenance_manifest=""

  core_build_dir="$(resolve_configured_core_build_dir "${OFFICIAL_BUILD_DIR}")"
  PERF_CORE_RUNTIME_PATH="$(resolve_core_runtime_library "${core_build_dir}")"
  PERF_CORE_PROVENANCE_REVISION=""
  PERF_CORE_RELEASE_TAG_VALUE=""
  PERF_CORE_DIRTY_VALUE="0"

  if [[ "${ZLINK_CORE_RELEASE_MODE:-0}" -eq 1 ]]; then
    provenance_manifest="${ZLINK_CORE_PACKAGE_PREFIX}/share/zlink/core-package-provenance.json"
    PERF_CORE_PROVENANCE_REVISION="$(
      sed -n 's/^[[:space:]]*"revision":[[:space:]]*"\([^"]*\)".*/\1/p' \
        "${provenance_manifest}" | head -n 1
    )"
    PERF_CORE_RELEASE_TAG_VALUE="$(
      sed -n 's/^[[:space:]]*"tag":[[:space:]]*"\([^"]*\)".*/\1/p' \
        "${provenance_manifest}" | head -n 1
    )"
  else
    PERF_CORE_PROVENANCE_REVISION="$(git -C "${ROOT_DIR}" rev-parse HEAD)"
    if ! git -C "${ROOT_DIR}" diff --quiet -- core; then
      PERF_CORE_DIRTY_VALUE="1"
    fi
  fi
}

print_core_runtime_binding() {
  local build_dir="${1:-${OFFICIAL_BUILD_DIR}}"
  local core_build_dir=""
  local runtime_lib=""
  core_build_dir="$(resolve_configured_core_build_dir "${build_dir}")"
  if runtime_lib="$(resolve_core_runtime_library "${core_build_dir}")"; then
    echo "Perf core build dir: ${core_build_dir}"
    echo "Perf runtime libzlink: ${runtime_lib}"
    return 0
  fi
  echo "Perf core build dir: ${core_build_dir}"
  echo "Error: core runtime library not found under ${core_build_dir}." >&2
  echo "Build core first so bindings/cpp/perf can link the intended runtime." >&2
  return 1
}

ensure_core_runtime_not_stale() {
  local build_dir="${1:-${OFFICIAL_BUILD_DIR}}"
  local command_name="${2:-run_benchmarks.sh}"
  local core_build_dir=""
  local core_source_dir="${ROOT_DIR}/core"
  local runtime_lib=""
  local newer_source=""
  core_build_dir="$(resolve_configured_core_build_dir "${build_dir}")"
  if [[ "${ZLINK_CORE_RELEASE_MODE:-0}" -eq 1 ]]; then
    if ! runtime_lib="$(resolve_core_runtime_library "${core_build_dir}")"; then
      echo "Error: Core release runtime not found under ${core_build_dir}." >&2
      return 1
    fi
    echo "Perf Core release prefix: ${core_build_dir}"
    echo "Perf runtime libzlink: ${runtime_lib}"
    return 0
  fi
  if ! runtime_lib="$(resolve_core_runtime_library "${core_build_dir}")"; then
    echo "Error: core runtime library not found under ${core_build_dir}." >&2
    echo "Build core first before running bindings/cpp/perf benchmarks." >&2
    return 1
  fi

  # A binding worktree may intentionally link core/build from the main
  # checkout. Compare that runtime with the source tree that owns the resolved
  # build directory; worktree checkout mtimes do not describe its freshness.
  local build_owner_source=""
  build_owner_source="$(dirname "${core_build_dir}")"
  if [[ -d "${build_owner_source}/src" && -d "${build_owner_source}/include" ]]; then
    core_source_dir="${build_owner_source}"
  fi

  newer_source="$(
    find \
      "${core_source_dir}/src" \
      "${core_source_dir}/include" \
      -type f -newer "${runtime_lib}" -print -quit 2>/dev/null || true
  )"
  if [[ -n "${newer_source}" ]]; then
    echo "Error: stale core runtime detected for bindings/cpp/perf." >&2
    echo "  runtime: ${runtime_lib}" >&2
    echo "  newer source: ${newer_source}" >&2
    echo "Rebuild core/build before running ${command_name}." >&2
    return 1
  fi
  return 0
}

ensure_cpp_core_build_runtime_enabled() {
  local build_dir="${1:-${OFFICIAL_BUILD_DIR}}"
  local cache_path="${build_dir}/CMakeCache.txt"
  local configured=""
  if [[ -f "${cache_path}" ]]; then
    configured="$(
      sed -n 's/^ZLINK_CPP_USE_CORE_BUILD_RUNTIME:BOOL=//p' "${cache_path}" \
        | tail -n 1
    )"
  fi
  if [[ "${configured}" != "ON" ]]; then
    echo "Error: C++ perf build is not configured to use core/build runtime." >&2
    echo "  cache: ${cache_path}" >&2
    echo "Reconfigure without --reuse-build or pass -DZLINK_CPP_USE_CORE_BUILD_RUNTIME=ON." >&2
    return 1
  fi
  return 0
}
