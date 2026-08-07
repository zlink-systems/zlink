#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
configuration="${CONFIGURATION:-Release}"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [--core-prefix ABSOLUTE_DIR]

Builds the C++ binding against the installed Core 0.10.1 local package and
installs it below .artifacts/wsl/install/zlink-cpp/0.10.1.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

[[ "$core_prefix" = /* ]] || { echo "--core-prefix must be absolute" >&2; exit 2; }
core_prefix="$(readlink -f "$core_prefix")"
version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
package_version="$(sed -n 's/^project(zlink_cpp VERSION \([0-9.]*\).*/\1/p' "$repo_root/bindings/cpp/CMakeLists.txt" | head -n1)"
[[ "$package_version" = "$version" ]] || {
  echo "C++ binding version $package_version does not match Core $version" >&2
  exit 1
}
prefix="$artifact_root/install/zlink-cpp/$version"
build_dir="$artifact_root/build/bindings-cpp-$version"
rm -rf "$prefix"
cmake -S "$repo_root/bindings/cpp" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="$configuration" \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DZLINK_CPP_CORE_PACKAGE_PREFIX="$core_prefix" \
  -DZLINK_CPP_BUILD_TESTS=OFF -DZLINK_CPP_BUILD_SAMPLES=OFF
cmake --build "$build_dir" --parallel "${ZLINK_BINDING_BUILD_JOBS:-4}"
cmake --install "$build_dir"

[[ -f "$prefix/include/zlink.hpp" ]] || {
  echo "C++ package headers are missing: $prefix" >&2
  exit 1
}
[[ -f "$prefix/lib/libzlink_cpp.a" ]] || {
  echo "C++ package library is missing: $prefix" >&2
  exit 1
}
echo "C++ local package: $prefix"
