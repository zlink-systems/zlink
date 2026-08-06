#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
bindings_dir="$repo_root/bindings/node"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [--core-prefix ABSOLUTE_DIR]

Builds and packs @zlink-systems/zlink@0.11.0 with the Core 0.11.0 native
runtime. The native ABI SONAME is libzlink.so.0.
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
export ZLINK_CORE_INSTALL_PREFIX="$core_prefix"
version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
package_version="$(node -p "require('$bindings_dir/package.json').version")"
[[ "$package_version" = "$version" ]] || {
  echo "Node package version $package_version does not match Core $version" >&2
  exit 1
}
core_library="$core_prefix/lib/libzlink.so"
manifest="$core_prefix/share/zlink/core-package-provenance.json"
host_arch="$(uname -m)"
case "$host_arch" in
  x86_64|amd64) node_arch=x64 ;;
  aarch64|arm64) node_arch=arm64 ;;
  *) echo "Unsupported Node host architecture: $host_arch" >&2; exit 2 ;;
esac

mkdir -p "$artifact_root/npm"
(
  cd "$bindings_dir"
  rm -rf prebuilds provenance
  ZLINK_SKIP_NATIVE_INSTALL=1 npm ci
  npm run build
  npm run rebuild-native
  prebuild_dir="prebuilds/linux-$node_arch"
  mkdir -p "$prebuild_dir" provenance
  cp build/Release/zlink.node "$prebuild_dir/zlink.node"
  cp "$core_library" "$prebuild_dir/libzlink.so.$version"
  cp "$core_prefix/lib/libzlink.so.0" "$prebuild_dir/libzlink.so.0"
  cp "$manifest" provenance/core-package-provenance.json
  ZLINK_CORE_VERSION="$version" npm run verify:prebuilds
  npm pack --pack-destination "$artifact_root/npm"
)

package="$artifact_root/npm/zlink-systems-zlink-$version.tgz"
[[ -f "$package" ]] || { echo "Node package is missing: $package" >&2; exit 1; }
echo "Node local package: $package"
