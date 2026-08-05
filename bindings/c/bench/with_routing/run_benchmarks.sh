#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() {
  cat <<'USAGE'
Usage: bindings/c/bench/with_routing/run_benchmarks.sh [options]

Run router compare benchmark: zlink vs libzmq vs gRPC.
Executes 1:1 echo then N:1 echo and prints combined results.

Options:
  --runs N              Iterations per configuration (default: 3).
  --clients N           N:1 test client count (default: 100).
  --build-dir PATH      Override build directory.
  --zlink-only          Run only zlink and compare with cached baselines.
  --refresh-cache       Refresh baseline caches.
  --run-cooldown-ms N   Sleep between runs in ms (default: 3000).
  --single              Run only 1:1 echo phase.
  --multi               Run only N:1 echo phase.
  --help                Show this help.
USAGE
}

RUNS=3
CLIENTS=100
BUILD_DIR=""
ZLINK_ONLY=0
REFRESH_CACHE=0
RUN_COOLDOWN_MS=3000
PHASE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --runs)
      if [[ $# -lt 2 ]]; then
        echo "Error: --runs requires a value." >&2
        exit 1
      fi
      RUNS="$2"
      shift 2
      ;;
    --clients)
      if [[ $# -lt 2 ]]; then
        echo "Error: --clients requires a value." >&2
        exit 1
      fi
      CLIENTS="$2"
      shift 2
      ;;
    --build-dir)
      if [[ $# -lt 2 ]]; then
        echo "Error: --build-dir requires a value." >&2
        exit 1
      fi
      BUILD_DIR="$2"
      shift 2
      ;;
    --zlink-only)
      ZLINK_ONLY=1
      shift
      ;;
    --refresh-cache)
      REFRESH_CACHE=1
      shift
      ;;
    --run-cooldown-ms)
      if [[ $# -lt 2 ]]; then
        echo "Error: --run-cooldown-ms requires a value." >&2
        exit 1
      fi
      RUN_COOLDOWN_MS="$2"
      shift 2
      ;;
    --single)
      PHASE="single"
      shift
      ;;
    --multi)
      PHASE="multi"
      shift
      ;;
    *)
      echo "Error: unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

# --- Resolve build directory and auto-build if needed ---
CORE_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)/core"
if [[ -z "${BUILD_DIR}" ]]; then
  BUILD_DIR="${CORE_DIR}/build"
fi

RC_TARGETS=(bench_rc_zlink_server bench_rc_zlink_client
            bench_rc_zmq_server bench_rc_zmq_client
            bench_rc_grpc_server bench_rc_grpc_client)

need_build=0
for t in "${RC_TARGETS[@]}"; do
  if [[ ! -x "${BUILD_DIR}/bin/${t}" ]]; then
    need_build=1
    break
  fi
done

if [[ "${need_build}" -eq 1 ]]; then
  echo "  Some benchmark binaries missing — building..."
  cmake --build "${BUILD_DIR}" --target "${RC_TARGETS[@]}" -j"$(nproc)"
  echo "  Build complete."
fi

PY_ARGS=()
PY_ARGS+=(--runs "${RUNS}")
PY_ARGS+=(--clients "${CLIENTS}")
PY_ARGS+=(--run-cooldown-ms "${RUN_COOLDOWN_MS}")

if [[ -n "${BUILD_DIR}" ]]; then
  PY_ARGS+=(--build-dir "${BUILD_DIR}")
fi
if [[ "${ZLINK_ONLY}" -eq 1 ]]; then
  PY_ARGS+=(--zlink-only)
fi
if [[ "${REFRESH_CACHE}" -eq 1 ]]; then
  PY_ARGS+=(--refresh-cache)
fi
if [[ -n "${PHASE}" ]]; then
  PY_ARGS+=(--phase "${PHASE}")
fi

echo "=== Router Compare Benchmark ==="
echo "  runs=${RUNS}, clients=${CLIENTS}"

exec python3 "${SCRIPT_DIR}/run_comparison.py" "${PY_ARGS[@]}"
