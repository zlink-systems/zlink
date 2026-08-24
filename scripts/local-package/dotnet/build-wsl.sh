#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
python3 "$repo_root/scripts/local-package/sync-version.py" --check >/dev/null
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
configuration="${CONFIGURATION:-Release}"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [--core-prefix ABSOLUTE_DIR]

Creates Systems.Zlink.<repository-version>.nupkg with the exact matching Core
Linux runtime.
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
verify_package_entry() {
  local entry="$1"
  if command -v unzip >/dev/null 2>&1; then
    unzip -Z1 "$package" | grep -Fxq "$entry"
  else
    PACKAGE="$package" ENTRY="$entry" python3 - <<'PY'
import os
import zipfile

package = os.environ["PACKAGE"]
entry = os.environ["ENTRY"]
with zipfile.ZipFile(package) as archive:
    if entry not in archive.namelist():
        raise SystemExit(f"NuGet package entry is missing: {entry}")
PY
  fi
}

verify_package_entry "runtimes/linux-x64/native/libzlink.so.0"
verify_package_entry "runtimes/linux-x64/native/libzlink.so.$version"
verify_package_entry "provenance/core-package-provenance.json"
echo ".NET local package: $package"
