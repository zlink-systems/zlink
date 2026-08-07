#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-$artifact_root/install/zlink-core/$version}"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [core] [c] [cpp] [dotnet] [go] [java] [node] [python] [rust]

With no language arguments, builds Core and all eight first-party bindings at
version 0.10.1 into .artifacts/wsl. External package registries are not used.
EOF
}

if [[ "$#" -gt 0 && ( "$1" = "-h" || "$1" = "--help" ) ]]; then
  usage
  exit 0
fi

if [[ "$#" -eq 0 ]]; then
  set -- c cpp dotnet go java node python rust
fi

mkdir -p "$artifact_root"
if [[ ! -f "$core_prefix/share/zlink/core-package-provenance.json" || "${ZLINK_REBUILD_CORE:-1}" = "1" ]]; then
  ZLINK_LOCAL_PACKAGE_ROOT="$artifact_root" \
    "$script_dir/core/build-wsl.sh"
fi
core_prefix="$(readlink -f "$core_prefix")"
export ZLINK_LOCAL_PACKAGE_ROOT="$artifact_root"
export ZLINK_CORE_PACKAGE_PREFIX="$core_prefix"

"$script_dir/native/sync-local-core-libs.sh" c cpp dotnet go java node python rust

for lang in "$@"; do
  case "$lang" in
    c|cpp|dotnet|go|java|node|python|rust)
      echo "-- building local $lang package at $version"
      "$script_dir/$lang/build-wsl.sh" --core-prefix "$core_prefix"
      ;;
    core)
      ;;
    *)
      echo "Unknown language: $lang" >&2
      usage >&2
      exit 2
      ;;
  esac
done

echo "-- local packages are under $artifact_root"
