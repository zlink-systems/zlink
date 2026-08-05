#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
out_dir="$artifact_root/npm"
bindings_dir="$repo_root/bindings/node"
core_prefix="${ZLINK_CORE_INSTALL_PREFIX:-}"
approved_core_provenance_sha256="${ZLINK_APPROVED_CORE_PROVENANCE_SHA256:-}"

if [[ "$core_prefix" != /* ]]; then
  echo "ZLINK_CORE_INSTALL_PREFIX must name an absolute installed Core 11 package prefix" >&2
  exit 2
fi
core_prefix="$(readlink -f "$core_prefix")"
export ZLINK_CORE_INSTALL_PREFIX="$core_prefix"

if [[ ! "$approved_core_provenance_sha256" =~ ^[0-9a-f]{64}$ ]]; then
  echo "ZLINK_APPROVED_CORE_PROVENANCE_SHA256 must contain the reviewed Core package provenance SHA-256" >&2
  exit 2
fi

package_version="$(node -p "require('$bindings_dir/package.json').version")"
core_version="$(node "$bindings_dir/scripts/resolve_core.js" version)"
core_library="$(node "$bindings_dir/scripts/resolve_core.js" library)"
core_manifest="$core_prefix/share/zlink/core-package-provenance.json"
actual_core_provenance_sha256="$(sha256sum "$core_manifest" | awk '{print $1}')"
if [[ "$actual_core_provenance_sha256" != "$approved_core_provenance_sha256" ]]; then
  echo "Installed Core provenance does not match the R2-approved package" >&2
  exit 1
fi
package_line="${package_version%.*}"
core_line="${core_version%.*}"
package_patch="${package_version##*.}"
core_patch="${core_version##*.}"
if [[ "$package_version" != 11.* || "$package_line" != "$core_line"
      || ! "$package_patch" =~ ^[0-9]+$ || ! "$core_patch" =~ ^[0-9]+$
      || "$package_patch" -lt "$core_patch" ]]; then
  echo "Node package $package_version must use Core $core_version major.minor and an equal or newer patch" >&2
  exit 1
fi

host_arch="$(uname -m)"
case "$host_arch" in
  x86_64|amd64) node_arch="x64" ;;
  aarch64|arm64) node_arch="arm64" ;;
  *) echo "Unsupported Node local package host architecture: $host_arch" >&2; exit 2 ;;
esac

mkdir -p "$out_dir"
(
  cd "$bindings_dir"
  cleanup_generated_package_inputs() {
    rm -rf prebuilds provenance
  }
  trap cleanup_generated_package_inputs EXIT
  ZLINK_SKIP_NATIVE_INSTALL=1 npm ci
  npm run build
  npm run rebuild-native

  prebuild_dir="prebuilds/linux-$node_arch"
  rm -rf prebuilds provenance
  mkdir -p "$prebuild_dir" provenance
  cp build/Release/zlink.node "$prebuild_dir/zlink.node"
  cp "$core_library" "$prebuild_dir/libzlink.so.$core_version"
  cp "$core_library" "$prebuild_dir/libzlink.so.${core_version%%.*}"
  cp "$core_manifest" provenance/core-package-provenance.json

  ZLINK_CORE_VERSION="$core_version" npm run verify:prebuilds
  package_file="$(npm pack --pack-destination "$out_dir" "$@" | tail -n 1)"

  consumer_dir="$(mktemp -d)"
  (
    trap 'rm -rf "$consumer_dir"' EXIT
    cd "$consumer_dir"
    npm init -y >/dev/null
    npm install "$out_dir/$package_file" >/dev/null
    node -e "const z=require('@zlink-systems/zlink'); if(!z.createContext) process.exit(1)"
    node --input-type=module -e "import { createContext } from '@zlink-systems/zlink'; if(!createContext) process.exit(1)"
  )
  rm -rf "$consumer_dir"
  echo "$package_file"
)

echo "-- Node local npm tarball output: $out_dir"
echo "-- Node binding package version: $package_version"
echo "-- Installed Core package: $core_prefix"
