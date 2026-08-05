#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
artifact_root="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"

mkdir -p "$artifact_root"

if [ "$#" -eq 0 ]; then
  set -- dotnet java node cpp
fi

for lang in "$@"; do
  case "$lang" in
    dotnet|java|node|cpp)
      echo "-- building local $lang package into $artifact_root"
      ZLINK_LOCAL_PACKAGE_ROOT="$artifact_root" "$script_dir/$lang/build-wsl.sh"
      ;;
    *)
      echo "Unknown language: $lang" >&2
      echo "Usage: $0 [dotnet] [java] [node] [cpp]" >&2
      exit 2
      ;;
  esac
done

echo "-- local packages are under $artifact_root"
