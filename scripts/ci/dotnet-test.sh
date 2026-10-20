#!/usr/bin/env bash
# Runs `dotnet test` for one project/configuration/framework once. A failed
# test fails the job: CI does not rerun failed tests, because one failure is a
# defect to fix, not noise to retry away.
# A test that runs longer than the hang limit is dumped and aborted, so a hang
# fails the job with a dump in the uploaded results instead of running until
# the job is cancelled.
set -euo pipefail

project="$1"; configuration="$2"; framework="$3"
results="${RUNNER_TEMP:-/tmp}/zlink-test-results/$(basename "${project%.csproj}")-${configuration}-${framework}"
rm -rf "$results"; mkdir -p "$results"

dotnet test "$project" -c "$configuration" -f "$framework" --no-build \
  --logger "trx;LogFileName=test.trx" --results-directory "$results" \
  --blame-hang --blame-hang-timeout 10m --blame-hang-dump-type full
