#!/usr/bin/env bash
set -euo pipefail
cpp=false
dotnet=false
java=false
node=false
core_version_changed=false
paths=$(mktemp)
trap 'rm -f "$paths"' EXIT
git diff --name-only --no-renames -z "$1" > "$paths"
while IFS= read -r -d '' path; do
  case "$path" in
    VERSION)
      core_version_changed=true
      cpp=true
      dotnet=true
      java=true
      node=true
      ;;
    framework/runtime/*|framework/doc/framework/common/spec/*|framework/doc/framework/common/e2e/*|bindings/*|core/*|scripts/ci/*|.github/workflows/*)
      cpp=true
      dotnet=true
      java=true
      node=true
      ;;
    framework/languages/cpp/*) cpp=true ;;
    framework/languages/dotnet/*) dotnet=true ;;
    framework/doc/contract-inventory/jvm-api-snapshots/java.api.sha256) java=true ;;
    framework/languages/java/*) java=true ;;
    framework/languages/node/*) node=true ;;
  esac
done < "$paths"
{
  echo "cpp=$cpp"
  echo "dotnet=$dotnet"
  echo "java=$java"
  echo "node=$node"
  echo "core_version_changed=$core_version_changed"
} >> "${GITHUB_OUTPUT:-/dev/stdout}"
echo "Selected languages: cpp=$cpp dotnet=$dotnet java=$java node=$node core_version_changed=$core_version_changed"
