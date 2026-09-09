#!/usr/bin/env bash
# Source this script to retain VCPKG_ROOT and VCPKG_OVERLAY_PORTS in this shell.
zlink_bootstrap_cpp() {
  local use_conan=0 arg repo_root
  for arg in "$@"; do
    case "$arg" in
      --conan) use_conan=1 ;;
      --help|-h) printf '%s\n' 'Usage: source scripts/dev/bootstrap-cpp.sh [--conan]'; return 0 ;;
      *) printf 'Unknown argument: %s\n' "$arg" >&2; return 2 ;;
    esac
  done
  if [[ "$(uname -s)" == Darwin && "$(uname -m)" != arm64 ]]; then
    printf '%s\n' 'Intel Mac is not supported; use an Apple Silicon shell.' >&2
    return 1
  fi
  if (( use_conan )); then
    command -v conan >/dev/null || { printf '%s\n' 'Install Conan 2 first.' >&2; return 1; }
    conan profile detect || return
    printf '%s\n' 'In a standalone sample: conan install . --output-folder=build/conan --build=missing -s build_type=Debug' 'Then: cmake --preset conan'
    return 0
  fi
  repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)" || return
  export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/.cache/zlink/vcpkg}"
  if [[ ! -d "$VCPKG_ROOT" ]]; then
    mkdir -p "$(dirname "$VCPKG_ROOT")" || return
    git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT" || return
  fi
  if [[ ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
    printf 'VCPKG_ROOT is not a vcpkg checkout: %s\n' "$VCPKG_ROOT" >&2
    return 1
  fi
  if [[ ! -x "$VCPKG_ROOT/vcpkg" ]]; then
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics || return
  fi
  if [[ -d "$repo_root/vcpkg/ports" ]]; then
    case ":${VCPKG_OVERLAY_PORTS:-}:" in
      *":$repo_root/vcpkg/ports:"*) ;;
      *) export VCPKG_OVERLAY_PORTS="$repo_root/vcpkg/ports${VCPKG_OVERLAY_PORTS:+:$VCPKG_OVERLAY_PORTS}" ;;
    esac
  fi
  "$VCPKG_ROOT/vcpkg" version || return
  printf 'VCPKG_ROOT=%s\n' "$VCPKG_ROOT"
  printf '%s\n' 'Next: cmake --preset dev (workspace) or cmake --preset linux-ninja / macos-ninja (sample).'
  if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    printf '%s\n' 'Run with source to retain the exported environment in your shell.'
  fi
}
zlink_bootstrap_cpp "$@"
