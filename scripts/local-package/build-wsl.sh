#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
core_version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
binding_version="$(sed -n 's/^ZLINK_BINDINGS_VERSION=//p' "$repo_root/BINDINGS_VERSION")"
release_version="${ZLINK_CORE_RELEASE_VERSION:-$core_version}"
language_args=()
version_action="build"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [options] [c] [cpp] [dotnet] [go] [java] [node] [python] [rust]

With no language arguments, builds all eight first-party bindings at
BINDINGS_VERSION into .artifacts/wsl. By default the exact matching Core
release is downloaded and verified — a published core/vVERSION GitHub release
is a prerequisite. There is no local-core bypass.

Options:
  --core-version VERSION       Core release version (default: VERSION)
  --sync-versions              Sync managed Core/binding values and exit
  --verify-versions            Verify managed Core/binding values and exit
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-version) release_version="${2:-}"; shift 2 ;;
    --sync-versions) version_action="sync"; shift ;;
    --verify-versions) version_action="verify"; shift ;;
    -h|--help) usage; exit 0 ;;
    c|cpp|dotnet|go|java|node|python|rust|core)
      language_args+=("$1"); shift ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

case "$version_action" in
  sync)
    python3 "$script_dir/sync-version.py" --write
    python3 "$script_dir/sync-version.py" --check
    exit 0
    ;;
  verify)
    python3 "$script_dir/sync-version.py" --check
    exit 0
    ;;
  build)
    python3 "$script_dir/sync-version.py" --write
    python3 "$script_dir/sync-version.py" --check
    ;;
esac

if [[ "${#language_args[@]}" -eq 0 ]]; then
  language_args=(c cpp dotnet go java node python rust)
fi

[[ "$release_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "--core-version must be MAJOR.MINOR.PATCH: $release_version" >&2
  exit 2
}
[[ "$release_version" = "$core_version" ]] || {
  echo "Requested Core release $release_version must match repository Core $core_version" >&2
  exit 1
}

mkdir -p "$artifact_root"
artifact_root="$(readlink -f "$artifact_root")"
release_core_prefix="$(bash "$script_dir/core/fetch-release.sh" --version "$release_version")"
core_prefix="$artifact_root/install/zlink-core/$core_version"
case "$core_prefix" in
  "$artifact_root"/*) ;;
  *) echo "Core release staging prefix escaped artifact root" >&2; exit 2 ;;
esac
rm -rf -- "$core_prefix"
mkdir -p "$(dirname "$core_prefix")"
cp -a "$release_core_prefix" "$core_prefix"
core_prefix="$(readlink -f "$core_prefix")"
export ZLINK_LOCAL_PACKAGE_ROOT="$artifact_root"
export ZLINK_CORE_PACKAGE_PREFIX="$core_prefix"
export ZLINK_CORE_VERSION="$release_version"
core_manifest="$core_prefix/share/zlink/core-package-provenance.json"
[[ -f "$core_manifest" ]] || {
  echo "Core package provenance is missing: $core_manifest" >&2
  exit 1
}
manifest_version="$(sed -n 's/^[[:space:]]*"version":[[:space:]]*"\([^"]*\)".*/\1/p' "$core_manifest" | head -n1)"
[[ "$manifest_version" = "$release_version" ]] || {
  echo "Core package version $manifest_version does not match $release_version" >&2
  exit 1
}

"$script_dir/native/sync-local-core-libs.sh" c cpp dotnet go java node python rust

for lang in "${language_args[@]}"; do
  case "$lang" in
    c|cpp|dotnet|go|java|node|python|rust)
      echo "-- building local $lang package at $binding_version using Core $release_version (release)"
      bash "$script_dir/$lang/build-wsl.sh" --core-prefix "$core_prefix"
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
