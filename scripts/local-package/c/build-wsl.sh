#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
python3 "$repo_root/scripts/local-package/sync-version.py" --check >/dev/null
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
configuration="${CONFIGURATION:-Release}"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
    -h|--help) echo "Usage: build-wsl.sh [--core-prefix ABSOLUTE_DIR]"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done

core_version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
binding_version="$(sed -n 's/^ZLINK_BINDINGS_VERSION=//p' "$repo_root/BINDINGS_VERSION")"
[[ "$core_prefix" = /* ]] || { echo "--core-prefix must be absolute" >&2; exit 2; }
core_prefix="$(readlink -f "$core_prefix")"
export ZLINK_CORE_PACKAGE_PREFIX="$core_prefix"
export ZLINK_CORE_VERSION="$core_version"
"$repo_root/scripts/local-package/native/sync-local-core-libs.sh" c

build_dir="$artifact_root/build/bindings-c-$binding_version"
cmake -S "$repo_root/bindings/c" -B "$build_dir" \
  -DCMAKE_BUILD_TYPE="$configuration" \
  -DZLINK_C_CORE_BUILD_DIR="$core_prefix" \
  -DZLINK_C_BUILD_TESTS=ON -DZLINK_C_REGISTER_CTEST=ON \
  -DZLINK_C_BUILD_SAMPLES=OFF
cmake --build "$build_dir" --parallel "${ZLINK_BINDING_BUILD_JOBS:-4}"
ctest --test-dir "$build_dir" --output-on-failure

out_dir="$artifact_root/c"
stage="$out_dir/zlink-c-$binding_version"
archive="$out_dir/zlink-c-$binding_version.tar.gz"
rm -rf "$stage" "$archive"
mkdir -p "$stage/include" "$stage/lib" "$stage/provenance"
cp -a "$repo_root/bindings/c/include/." "$stage/include/"
cp -a "$core_prefix/include/." "$stage/include/"
cp -L "$core_prefix/lib/libzlink.so.$core_version" "$stage/lib/libzlink.so.$core_version"
ln -s "libzlink.so.$core_version" "$stage/lib/libzlink.so.0"
ln -s "libzlink.so.0" "$stage/lib/libzlink.so"
cp "$core_prefix/share/zlink/core-package-provenance.json" \
  "$stage/provenance/core-package-provenance.json"
tar -C "$out_dir" -czf "$archive" "zlink-c-$binding_version"
rm -rf "$stage"
echo "C local package: $archive"
