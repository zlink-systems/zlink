#!/usr/bin/env bash
# Machine prerequisites for the local integration gates.  This file is sourced
# by common.sh so the selected vcpkg and formatter locations reach the gate.

gate_env_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

gate_java_major() { # <java executable> -> major, or nothing
  local line
  line="$("$1" -version 2>&1 | sed -n '1p')" || return 1
  [[ "$line" =~ \"([0-9]+)([.\"]) ]] || return 1
  printf '%s\n' "${BASH_REMATCH[1]}"
}

check_env() {
  local missing=0 java java_major node_major cmake_version browser_root java_candidate

  export VCPKG_OVERLAY_PORTS="${VCPKG_OVERLAY_PORTS:-$gate_env_root/vcpkg/ports}"

  java=""
  for java_candidate in "${JAVA_HOME:+$JAVA_HOME/bin/java}" "$(command -v java 2>/dev/null || true)"; do
    [[ -x "$java_candidate" ]] || continue
    java_major="$(gate_java_major "$java_candidate" 2>/dev/null || true)"
    [[ "$java_major" =~ ^[0-9]+$ ]] && (( java_major >= 25 )) && { java="$java_candidate"; break; }
  done
  if [[ -n "$java" ]]; then
    echo "ok JDK 25"
  else
    echo "missing JDK 25: sudo apt install openjdk-25-jdk"
    ((missing++))
  fi

  # A bootstrapped Linux tree. A Windows tree reached through /mnt has vcpkg.cmake too but
  # its binaries and triplets are for Windows, so it is rejected by path.
  if [[ -n "${VCPKG_ROOT:-}" && "$VCPKG_ROOT" != /mnt/* && -x "$VCPKG_ROOT/vcpkg"       && -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" && -d "$VCPKG_OVERLAY_PORTS" ]]; then
    echo "ok VCPKG_ROOT"
  else
    echo "missing VCPKG_ROOT (bootstrapped Linux tree, not under /mnt): git clone https://github.com/microsoft/vcpkg \"$HOME/.cache/zlink/vcpkg\" && \"$HOME/.cache/zlink/vcpkg/bootstrap-vcpkg.sh\" && export VCPKG_ROOT=\"$HOME/.cache/zlink/vcpkg\""
    ((missing++))
  fi

  case "$(uname -s)" in
    Darwin) browser_root="$HOME/Library/Caches/ms-playwright" ;;
    MINGW*|MSYS*|CYGWIN*) browser_root="${LOCALAPPDATA:-}/ms-playwright" ;;
    *) browser_root="${XDG_CACHE_HOME:-$HOME/.cache}/ms-playwright" ;;
  esac
  if [[ -d "$browser_root" ]] && find "$browser_root" -maxdepth 1 -type d -name 'chromium-*' -print -quit | grep -q .; then
    echo "ok Playwright Chromium"
  else
    echo "missing Playwright Chromium: cd \"$gate_env_root/framework/languages/node\" && npm run browser:install"
    ((missing++))
  fi

  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    echo "ok docker"
  else
    echo "missing docker: sudo apt install docker.io && sudo systemctl enable --now docker"
    ((missing++))
  fi

  if command -v dotnet >/dev/null 2>&1; then
    echo "ok dotnet"
  else
    echo "missing dotnet: sudo apt install dotnet-sdk-8.0"
    ((missing++))
  fi

  node_major=""
  command -v node >/dev/null 2>&1 && node_major="$(node -p 'process.versions.node.split(".")[0]' 2>/dev/null || true)"
  if [[ "$node_major" =~ ^[0-9]+$ ]] && (( node_major >= 20 )); then
    echo "ok node >= 20"
  else
    echo "missing node >= 20: curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash - && sudo apt install nodejs"
    ((missing++))
  fi

  cmake_version="$(cmake --version 2>/dev/null | sed -n '1s/.* \([0-9][0-9.]*\)$/\1/p')"
  if [[ "$cmake_version" =~ ^[0-9]+\.[0-9]+ ]] && [[ "$(printf '%s\n3.20\n' "$cmake_version" | sort -V | head -n1)" == 3.20 ]]; then
    echo "ok cmake >= 3.20"
  else
    echo "missing cmake >= 3.20: sudo apt install cmake"
    ((missing++))
  fi

  if command -v ninja >/dev/null 2>&1; then
    echo "ok ninja"
  else
    echo "missing ninja: sudo apt install ninja-build"
    ((missing++))
  fi

  ((missing == 0))
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  check_env "$@"
fi
