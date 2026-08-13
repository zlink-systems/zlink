#!/usr/bin/env bash
#
# sync-local-core-libs.sh — copy Core headers and runtime from a verified
# Core prefix into binding workspaces. When no prefix is supplied, retain the
# local core/build fallback for in-progress Core development.
#
# This is for local development only. The committed contents of
# bindings/*/native/ are produced by the release CI from a tagged version of
# the core library; do NOT commit the files written by this script. Run
# `git restore bindings/*/native/libzlink.so*` (or equivalent) before staging
# unrelated changes for commit.
#
# Usage:
#   scripts/local-package/native/sync-local-core-libs.sh [binding ...]
#
# With no arguments all bindings are synchronized. Pass binding names to
# limit the update, for example: cpp dotnet java node.
#
# Optional environment overrides:
#   ZLINK_CORE_PACKAGE_PREFIX — verified Core install prefix. The prefix
#                                contains include/, lib/, and provenance.
#   CORE_LIB_DIR          — directory containing the Core shared library.
#   CORE_INCLUDE_DIR      — directory containing Core public headers.
#                           Without a package prefix, defaults are core/build/lib
#                           and core/include.
#   ZLINK_CORE_VERSION    — Core version (default: core/version.sh).
#   ZLINK_CORE_ABI_MAJOR — ELF SONAME major (default: 0). This is separate
#                           from the release version in VERSION.
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
CORE_PREFIX="${ZLINK_CORE_PACKAGE_PREFIX:-}"
if [[ -n "$CORE_PREFIX" ]]; then
  [[ "$CORE_PREFIX" = /* ]] || {
    echo "ZLINK_CORE_PACKAGE_PREFIX must be absolute" >&2
    exit 2
  }
  CORE_PREFIX="$(readlink -f "$CORE_PREFIX")"
  CORE_LIB_DIR="${CORE_LIB_DIR:-$CORE_PREFIX/lib}"
  CORE_INCLUDE_DIR="${CORE_INCLUDE_DIR:-$CORE_PREFIX/include}"
else
  CORE_LIB_DIR="${CORE_LIB_DIR:-$ROOT_DIR/core/build/lib}"
  CORE_INCLUDE_DIR="${CORE_INCLUDE_DIR:-$ROOT_DIR/core/include}"
fi

case "$(uname -s)" in
  Linux) os="linux" ;;
  Darwin) os="macos" ;;
  *) echo "unsupported OS: $(uname -s)" >&2; exit 2 ;;
esac
if [[ "${os}" != "linux" ]]; then
  echo "this dev sync currently only handles Linux shared libraries" >&2
  exit 2
fi

case "$(uname -m)" in
  x86_64|amd64) arch_dash="x86_64"; arch_short="x64" ;;
  aarch64|arm64) arch_dash="aarch64"; arch_short="arm64" ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 2 ;;
esac

VERSION="${ZLINK_CORE_VERSION:-$(bash "${ROOT_DIR}/core/version.sh")}"
ABI_MAJOR="${ZLINK_CORE_ABI_MAJOR:-0}"
[[ "${ABI_MAJOR}" =~ ^[0-9]+$ ]] || {
  echo "ZLINK_CORE_ABI_MAJOR must be numeric" >&2
  exit 2
}

declare -A selected=()
if [[ "$#" -eq 0 ]]; then
  set -- c cpp dotnet go java node python rust
fi
for binding in "$@"; do
  case "${binding}" in
    c|cpp|dotnet|go|java|node|python|rust)
      selected["${binding}"]=1
      ;;
    *)
      echo "unsupported binding: ${binding}" >&2
      echo "expected one or more of: c cpp dotnet go java node python rust" >&2
      exit 2
      ;;
  esac
done

base="${CORE_LIB_DIR}/libzlink.so"
soname="${CORE_LIB_DIR}/libzlink.so.${ABI_MAJOR}"
versioned="${CORE_LIB_DIR}/libzlink.so.${VERSION}"

for f in "${base}" "${soname}" "${versioned}"; do
  if [[ ! -e "${f}" ]]; then
    echo "missing core artifact: ${f}" >&2
    if [[ -n "$CORE_PREFIX" ]]; then
      echo "Core package prefix is incomplete: $CORE_PREFIX" >&2
    else
      echo "build core first (e.g. cmake --build core/build)." >&2
    fi
    exit 1
  fi
done

if [[ -n "$CORE_PREFIX" ]]; then
  [[ -f "$CORE_PREFIX/share/zlink/core-package-provenance.json" ]] || {
    echo "Core package provenance is missing: $CORE_PREFIX/share/zlink/core-package-provenance.json" >&2
    exit 1
  }
fi

copy_header_files() {
  local source_dir="$1"
  local target_dir="$2"
  local source_file target_file mode
  for source_file in "${source_dir}"/*.h; do
    [[ -f "${source_file}" ]] || continue
    target_file="${target_dir}/$(basename "${source_file}")"
    mode=""
    if [[ -e "${target_file}" ]]; then
      mode="$(stat -c '%a' "${target_file}")"
    fi
    cp -f "${source_file}" "${target_file}"
    if [[ -n "${mode}" ]]; then
      chmod "${mode}" "${target_file}"
    fi
  done
}

copy_public_headers() {
  local dir="$1"

  mkdir -p "${dir}"
  mkdir -p "${dir}/zlink"

  # Core 0.11.0 owns only the raw C headers. Remove the retired service tree and
  # replace each Core-owned lowercase group without touching binding-owned
  # trees such as the C++ Contracts directory.
  rm -rf "${dir}/zlink/service"
  rm -f "${dir}/zlink.h" "${dir}/zlink_enum.h" "${dir}/zlink_errno.h"
  rm -f "${dir}"/zlink/*.h
  copy_header_files "$CORE_INCLUDE_DIR" "${dir}"
  copy_header_files "$CORE_INCLUDE_DIR/zlink" "${dir}/zlink"
  for source_group in "$CORE_INCLUDE_DIR"/zlink/*/; do
    [[ -d "${source_group}" ]] || continue
    group="$(basename "${source_group}")"
    rm -rf "${dir}/zlink/${group}"
    cp -a "${source_group}" "${dir}/zlink/${group}"
  done
}

copy_libs() {
  local dir="$1"
  mkdir -p "${dir}"
  rm -f "${dir}/libzlink.so" "${dir}/libzlink.so.${ABI_MAJOR}"
  find "${dir}" -maxdepth 1 -type f -name 'libzlink.so.*' -delete
  find "${dir}" -maxdepth 1 -type l -name 'libzlink.so.*' -delete
  install -m 0755 "${versioned}" "${dir}/libzlink.so.${VERSION}"
  ln -sfn "libzlink.so.${VERSION}" "${dir}/libzlink.so.${ABI_MAJOR}"
  ln -sfn "libzlink.so.${ABI_MAJOR}" "${dir}/libzlink.so"
}

dirs=()
header_dirs=()
if [[ -n "${selected[c]:-}" ]]; then
  header_dirs+=("${ROOT_DIR}/bindings/c/include")
fi
if [[ -n "${selected[cpp]:-}" ]]; then
  dirs+=(
    "${ROOT_DIR}/bindings/cpp/native/linux-${arch_short}"
    "${ROOT_DIR}/bindings/cpp/native/linux-${arch_dash}"
  )
  header_dirs+=("${ROOT_DIR}/bindings/cpp/include")
fi
if [[ -n "${selected[dotnet]:-}" ]]; then
  dirs+=("${ROOT_DIR}/bindings/dotnet/native/linux-${arch_short}")
fi
if [[ -n "${selected[go]:-}" ]]; then
  dirs+=(
    "${ROOT_DIR}/bindings/go/native/linux-${arch_short}"
    "${ROOT_DIR}/bindings/go/native/linux-${arch_dash}"
  )
  header_dirs+=("${ROOT_DIR}/bindings/go/include")
fi
if [[ -n "${selected[java]:-}" ]]; then
  dirs+=(
    "${ROOT_DIR}/bindings/java/native/linux-${arch_short}"
    "${ROOT_DIR}/bindings/java/native/linux-${arch_dash}"
    "${ROOT_DIR}/bindings/java/src/main/resources/native/linux-${arch_short}"
    "${ROOT_DIR}/bindings/java/src/main/resources/native/linux-${arch_dash}"
  )
fi
if [[ -n "${selected[node]:-}" ]]; then
  dirs+=(
    "${ROOT_DIR}/bindings/node/native/linux-${arch_short}"
    "${ROOT_DIR}/bindings/node/native/linux-${arch_dash}"
    "${ROOT_DIR}/bindings/node/prebuilds/linux-${arch_short}"
  )
fi
if [[ -n "${selected[python]:-}" ]]; then
  if [[ "${arch_dash}" != "x86_64" ]]; then
    echo "Python binding candidate supports Linux x86_64 only" >&2
    exit 2
  fi
  dirs+=("${ROOT_DIR}/bindings/python/src/zlink/native/linux-x86_64")
fi
if [[ -n "${selected[rust]:-}" ]]; then
  dirs+=(
    "${ROOT_DIR}/bindings/rust/native/linux-${arch_short}"
    "${ROOT_DIR}/bindings/rust/native/linux-${arch_dash}"
  )
  header_dirs+=("${ROOT_DIR}/bindings/rust/include")
fi

for dir in "${header_dirs[@]}"; do
  copy_public_headers "${dir}"
done

for dir in "${dirs[@]}"; do
  copy_libs "${dir}"
done

if [[ -n "$CORE_PREFIX" ]]; then
  echo "synced zlink headers and libzlink ${VERSION} from Core package $CORE_PREFIX into binding workspaces"
else
  echo "synced zlink headers and libzlink ${VERSION} from ${CORE_LIB_DIR} into binding workspaces"
fi
echo
echo "WARNING: bindings/*/native/libzlink.so* and binding package native"
echo "artifacts are release inputs. Do NOT"
echo "commit the files written by this script. Run"
echo "  git restore bindings/*/native/libzlink.so* bindings/*/src/**/native/libzlink.so* bindings/*/prebuilds/*/libzlink.so*"
echo "before staging unrelated changes."
