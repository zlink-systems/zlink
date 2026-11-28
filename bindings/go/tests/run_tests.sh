#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

passed=0
stage="setup"
trap 'status=$?; if (( status == 0 )); then
  printf "Go tests: passed (%d stages)\n" "$passed"
else
  printf "Go tests: failed during %s (%d stages passed, exit %d)\n" "$stage" "$passed" "$status" >&2
fi' EXIT

run_stage() {
  stage="$1"
  shift
  printf '==> %s\n' "$stage"
  "$@"
  passed=$((passed + 1))
}

export GODEBUG="${GODEBUG:+${GODEBUG},}cgocheck=1"
export GOEXPERIMENT="${GOEXPERIMENT:+${GOEXPERIMENT},}cgocheck2"

run_stage "go test all Go packages (cgo pointer checks enabled)" go test ./...
run_stage "go vet all Go packages" go vet ./...
run_stage "go test Go perf module" go -C perf test ./...
run_stage "go vet Go perf module" go -C perf vet ./...
run_stage "raw contract and hot-path guards" go test ./internal/native \
  -run 'Test(OptimizationGuard|RawCore11Allowlist|HotPathCostInventory|EveryPackagedCoreExportHasAnOwner)' -count=1
run_stage "sample smoke" "${ROOT_DIR}/samples/run_samples.sh"
