#!/usr/bin/env bash
# Builds the .NET with-grpc bench. Owner of the .NET build command; run_local.sh and build_all.sh call it.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../runner_common.sh
source "${ROOT_DIR}/../runner_common.sh"

# MSBuild node reuse leaves build servers running after the build; they inherit the caller's
# file descriptors, so a perf lock taken around the build stays held and the next measurement
# waits forever.
export MSBUILDDISABLENODEREUSE=1
bench_require_low_load
dotnet build "${ROOT_DIR}/WithGrpcBench.sln" -c Release
