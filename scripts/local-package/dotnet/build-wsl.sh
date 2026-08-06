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

Creates Systems.Zlink.0.11.0.nupkg with the exact Core 0.11.0 Linux runtime.
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
manifest="$core_prefix/share/zlink/core-package-provenance.json"
out_dir="$artifact_root/nuget"
mkdir -p "$out_dir"

dotnet pack "$repo_root/bindings/dotnet/src/Zlink/Zlink.csproj" \
  -c "$configuration" -o "$out_dir" \
  -p:ZLinkLinuxX64NativeRoot="$core_prefix/lib" \
  -p:ZLinkCoreVersion="$version" \
  -p:ZLinkCoreProvenancePath="$manifest"

package="$out_dir/Systems.Zlink.$version.nupkg"
[[ -f "$package" ]] || { echo "NuGet package is missing: $package" >&2; exit 1; }
unzip -Z1 "$package" | grep -Fxq "runtimes/linux-x64/native/libzlink.so.0"
unzip -Z1 "$package" | grep -Fxq "runtimes/linux-x64/native/libzlink.so.$version"
unzip -Z1 "$package" | grep -Fxq "provenance/core-package-provenance.json"
echo ".NET local package: $package"
