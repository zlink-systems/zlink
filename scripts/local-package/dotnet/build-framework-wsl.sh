#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(git -C "$script_dir" rev-parse --show-toplevel)"
export ZLINK_LOCAL_PACKAGE_ROOT="${ZLINK_LOCAL_PACKAGE_ROOT:-$repo_root/.artifacts/wsl}"
mkdir -p "$ZLINK_LOCAL_PACKAGE_ROOT/nuget"
core_prefix="$(bash "$repo_root/scripts/local-package/core/fetch-release.sh")"
bash "$script_dir/build-wsl.sh" --core-prefix "$core_prefix" >&2
bash "$repo_root/scripts/local-package/http-client/build-wsl.sh" dotnet >&2
for project in "$repo_root"/framework/languages/dotnet/src/*/*.csproj; do
  if grep -q '<IsPackable>true</IsPackable>' "$project"; then
    dotnet pack "$project" -c Release -p:PackageOutputPath="$ZLINK_LOCAL_PACKAGE_ROOT/nuget" >&2
  fi
done
printf '%s\n' "$ZLINK_LOCAL_PACKAGE_ROOT/nuget"
