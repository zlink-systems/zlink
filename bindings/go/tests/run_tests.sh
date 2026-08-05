#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "${ROOT_DIR}"

echo "==> go test all Go packages"
go test ./...

echo "==> go vet all Go packages"
go vet ./...

echo "==> raw contract and hot-path guards"
go test ./internal/native -run 'Test(OptimizationGuard|RawCore11Allowlist|HotPathCostInventory)' -count=1

"${ROOT_DIR}/samples/run_samples.sh"
