#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "${ROOT_DIR}/../.." && pwd)"
source "${REPO_ROOT}/bindings/tools/local_core_runtime.sh"
export CGO_CFLAGS="${CGO_CFLAGS:-} -I${ZLINK_CORE_INCLUDE_DIR}"
export CGO_LDFLAGS="${CGO_LDFLAGS:-} -L${ZLINK_CORE_LIB_DIR} -Wl,-rpath,${ZLINK_CORE_LIB_DIR}"
export LD_LIBRARY_PATH="${ZLINK_CORE_LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"

cd "${ROOT_DIR}"

echo "==> go test all Go packages"
go test ./...

echo "==> go vet all Go packages"
go vet ./...

echo "==> raw contract and hot-path guards"
go test ./internal/native -run 'Test(OptimizationGuard|RawCore11Allowlist|HotPathCostInventory)' -count=1

"${ROOT_DIR}/samples/run_samples.sh"
