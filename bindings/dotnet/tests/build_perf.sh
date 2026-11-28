#!/usr/bin/env bash
set -euo pipefail

PERF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../perf" && pwd)"
CONFIGURATION="${1:-Debug}"
SINGLE_PROJECT="${PERF_DIR}/single/Zlink.BindingBench/Zlink.BindingBench.csproj"
PACED_PROJECT="${PERF_DIR}/tests/Zlink.BindingBench.PacedAdmission/Zlink.BindingBench.PacedAdmission.csproj"

echo "[dotnet-tests] build single perf project"
dotnet build "${SINGLE_PROJECT}" -c "${CONFIGURATION}" -m:1

echo "[dotnet-tests] build multi perf and PacedAdmission projects"
dotnet build "${PACED_PROJECT}" -c "${CONFIGURATION}" -m:1

echo "[dotnet-tests] run PacedAdmission"
dotnet run --project "${PACED_PROJECT}" -c "${CONFIGURATION}" --no-build
