#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
PROJECT="${ROOT_DIR}/tests/Zlink.Tests/Zlink.Tests.csproj"
source "${ROOT_DIR}/../tools/local_core_runtime.sh"
zlink_export_local_core_runtime

echo "[dotnet-tests] building ${PROJECT}"
dotnet build "${PROJECT}" -m:1

zlink_sync_linux_native_dirs_by_find "${ROOT_DIR}/tests/Zlink.Tests/bin" '*linux-x64/native'

echo "[dotnet-tests] running ${PROJECT}"
if dotnet test "${PROJECT}" --no-build "$@" && \
    "${ROOT_DIR}/samples/run_samples.sh"; then
    echo "[dotnet-tests] PASS"
else
    status=$?
    echo "[dotnet-tests] FAIL (${status})"
    exit "${status}"
fi
