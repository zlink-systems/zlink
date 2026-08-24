#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
version="$(sed -n 's/^LIBZLINK_VERSION=//p' "$repo_root/VERSION")"
core_source="${ZLINK_CORE_SOURCE:-release}"
release_version="${ZLINK_CORE_RELEASE_VERSION:-$version}"
core_prefix="${ZLINK_CORE_PACKAGE_PREFIX:-}"
language_args=()
version_action="build"

usage() {
  cat <<'EOF'
Usage: build-wsl.sh [options] [c] [cpp] [dotnet] [go] [java] [node] [python] [rust]

With no language arguments, builds all eight first-party bindings at the
repository VERSION into .artifacts/wsl. By default the exact matching Core
release is downloaded and verified; set --core-source local when testing an
in-progress Core build.

Options:
  --core-source release|local  Core input mode (default: release)
  --core-version VERSION       Core release version (default: VERSION)
  --core-prefix ABSOLUTE_DIR   Use an already verified Core prefix
  --sync-versions              Sync managed Core/binding values and exit
  --verify-versions            Verify managed Core/binding values and exit
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core-source) core_source="${2:-}"; shift 2 ;;
    --core-version) release_version="${2:-}"; shift 2 ;;
    --core-prefix) core_prefix="${2:-}"; shift 2 ;;
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

case "$core_source" in
  release|local) ;;
  *) echo "--core-source must be release or local: $core_source" >&2; exit 2 ;;
esac
[[ "$release_version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || {
  echo "--core-version must be MAJOR.MINOR.PATCH: $release_version" >&2
  exit 2
}
[[ "$release_version" = "$version" ]] || {
  echo "Binding version $version must match Core release version $release_version" >&2
  exit 1
}

mkdir -p "$artifact_root"
artifact_root="$(readlink -f "$artifact_root")"
if [[ -n "$core_prefix" ]]; then
  [[ "$core_prefix" = /* ]] || {
    echo "--core-prefix or ZLINK_CORE_PACKAGE_PREFIX must be absolute" >&2
    exit 2
  }
  core_prefix="$(readlink -f "$core_prefix")"
elif [[ "$core_source" = release ]]; then
  release_core_prefix="$(bash "$script_dir/core/fetch-release.sh" --version "$release_version")"
  core_prefix="$artifact_root/install/zlink-core/$version"
  case "$core_prefix" in
    "$artifact_root"/*) ;;
    *) echo "Core release staging prefix escaped artifact root" >&2; exit 2 ;;
  esac
  rm -rf -- "$core_prefix"
  mkdir -p "$(dirname "$core_prefix")"
  cp -a "$release_core_prefix" "$core_prefix"
  core_prefix="$(readlink -f "$core_prefix")"
else
  core_prefix="$artifact_root/install/zlink-core/$version"
  if [[ ! -f "$core_prefix/share/zlink/core-package-provenance.json" || "${ZLINK_REBUILD_CORE:-1}" = "1" ]]; then
    ZLINK_LOCAL_PACKAGE_ROOT="$artifact_root" \
      bash "$script_dir/core/build-wsl.sh"
  fi
  core_prefix="$(readlink -f "$core_prefix")"
fi
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
      echo "-- building local $lang package at $version using Core $release_version ($core_source)"
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
