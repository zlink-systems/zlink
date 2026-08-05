#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
source "${REPO_ROOT}/bindings/tools/local_core_runtime.sh"
CORE_RUNTIME="${ZLINK_CORE_VERSIONED_LIB}"
cd "$ROOT_DIR"

REUSE_BUILD=0
CLEAN_BUILD=0
for arg in "$@"; do
  case "${arg}" in
    --reuse-build) REUSE_BUILD=1 ;;
    --clean-build) CLEAN_BUILD=1 ;;
  esac
done

if [ "${REUSE_BUILD}" -eq 1 ] && [ "${CLEAN_BUILD}" -eq 1 ]; then
  echo "Error: --reuse-build and --clean-build are mutually exclusive." >&2
  exit 1
fi

if [ "${CLEAN_BUILD}" -eq 1 ]; then
  rm -rf "$ROOT_DIR/dist-tools"
fi

if [ "${REUSE_BUILD}" -eq 0 ]; then
  npm run build
elif [ ! -f "$ROOT_DIR/dist-tools/perf/multi/run_benchmarks.js" ]; then
  echo "Error: --reuse-build requested but dist-tools output is missing." >&2
  exit 1
fi

if [ ! -f "$CORE_RUNTIME" ]; then
  echo "Error: core runtime is missing: $CORE_RUNTIME" >&2
  echo "Build it first with: cmake --build core/build" >&2
  exit 1
fi

if find "$REPO_ROOT/core/include" "$REPO_ROOT/core/src" -type f -newer "$CORE_RUNTIME" | grep -q .; then
  echo "Error: core runtime is older than core/include or core/src." >&2
  echo "Rebuild it first with: cmake --build core/build" >&2
  echo "Runtime checked: $CORE_RUNTIME" >&2
  exit 1
fi

echo "Perf runtime libzlink: $(realpath "$CORE_RUNTIME")"
export ZLINK_PERF_RUNTIME_LIBZLINK
ZLINK_PERF_RUNTIME_LIBZLINK="$(realpath "$CORE_RUNTIME")"

node dist-tools/perf/multi/run_benchmarks.js "$@"
