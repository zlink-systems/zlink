#!/usr/bin/env bash
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${ROOT}/../../.." && pwd)"
source "${REPO_ROOT}/bindings/tools/local_core_runtime.sh"
zlink_export_local_core_runtime

mapfile -t SAMPLES < <(
  dotnet sln "${ROOT}/Zlink.Samples.sln" list |
    awk '/\.csproj$/ && $0 !~ /^SampleCommon\// { print }'
)

if (( ${#SAMPLES[@]} == 0 )); then
  echo "No sample projects found in Zlink.Samples.sln" >&2
  exit 1
fi

passed=0
failed=0

for sample in "${SAMPLES[@]}"; do
  echo "RUN,$sample"
  if dotnet build "$ROOT/$sample" && zlink_sync_linux_native_dirs_by_find "$(dirname "$ROOT/$sample")/bin" '*linux-x64/native' && \
      timeout 60s dotnet run --no-build --project "$ROOT/$sample"; then
    echo "OK,$sample"
    passed=$((passed + 1))
  else
    echo "FAIL,$sample"
    failed=$((failed + 1))
  fi
done

echo "SUMMARY,passed,$passed,failed,$failed,total,${#SAMPLES[@]}"

if (( failed > 0 )); then
  exit 1
fi
