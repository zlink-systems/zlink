#!/usr/bin/env bash
# Runs one already-built .NET console check program once. Its exit code is the
# number of failed checks, so a nonzero exit fails the job. Like dotnet-test.sh,
# CI does not rerun it: one failure is a defect to fix.
set -euo pipefail

project="$1"; configuration="$2"; framework="$3"

limit=()
# macOS runners have no coreutils timeout; the job timeout still bounds the step there.
if command -v timeout >/dev/null 2>&1; then
  limit=(timeout --kill-after=30s 10m)
fi

${limit[@]+"${limit[@]}"} dotnet run --project "$project" -c "$configuration" -f "$framework" --no-build
