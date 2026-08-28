#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"

CORE_VERSION_OPTION=""
PART_COUNT="${PERF_PART_COUNT:-2}"
SCRIPT_ARGUMENTS=("$@")
FORWARD_ARGUMENTS=()
for ((argument_index = 0; argument_index < ${#SCRIPT_ARGUMENTS[@]}; ++argument_index)); do
  case "${SCRIPT_ARGUMENTS[argument_index]}" in
    --core-version)
      if (( argument_index + 1 >= ${#SCRIPT_ARGUMENTS[@]} )); then
        echo "Error: --core-version requires a version." >&2
        exit 1
      fi
      ((++argument_index))
      requested_core_version="${SCRIPT_ARGUMENTS[argument_index]}"
      ;;
    --core-version=*)
      requested_core_version="${SCRIPT_ARGUMENTS[argument_index]#--core-version=}"
      ;;
    --part-count)
      if (( argument_index + 1 >= ${#SCRIPT_ARGUMENTS[@]} )); then
        echo "Error: --part-count requires 1 or 2." >&2
        exit 1
      fi
      ((++argument_index))
      PART_COUNT="${SCRIPT_ARGUMENTS[argument_index]}"
      continue
      ;;
    --part-count=*)
      PART_COUNT="${SCRIPT_ARGUMENTS[argument_index]#--part-count=}"
      continue
      ;;
    *)
      FORWARD_ARGUMENTS+=("${SCRIPT_ARGUMENTS[argument_index]}")
      continue
      ;;
  esac
  if [[ ! "${requested_core_version}" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: --core-version must be MAJOR.MINOR.PATCH: ${requested_core_version:-<missing>}" >&2
    exit 1
  fi
  if [[ -n "${CORE_VERSION_OPTION}" && "${CORE_VERSION_OPTION}" != "${requested_core_version}" ]]; then
    echo "Error: --core-version may be specified only once." >&2
    exit 1
  fi
  CORE_VERSION_OPTION="${requested_core_version}"
done
set -- "${FORWARD_ARGUMENTS[@]+"${FORWARD_ARGUMENTS[@]}"}"
if [[ "${PART_COUNT}" != "1" && "${PART_COUNT}" != "2" ]]; then
  echo "Error: --part-count must be 1 or 2." >&2
  exit 1
fi
export PERF_PART_COUNT="${PART_COUNT}"

# Use the current workspace Core by default. An explicit --core-version selects
# the downloaded release package for that version instead.
if [[ -n "${CORE_VERSION_OPTION}" ]]; then
  if [[ -n "${ZLINK_CORE_SOURCE:-}" && "${ZLINK_CORE_SOURCE}" != "release" ]]; then
    echo "Error: --core-version cannot be combined with ZLINK_CORE_SOURCE=${ZLINK_CORE_SOURCE}." >&2
    exit 1
  fi
  export ZLINK_CORE_SOURCE=release
  export ZLINK_CORE_RELEASE_VERSION="${CORE_VERSION_OPTION}"
  export ZLINK_CORE_ALLOW_VERSION_MISMATCH=1
else
  export ZLINK_CORE_SOURCE="${ZLINK_CORE_SOURCE:-local}"
fi
source "${REPO_ROOT}/bindings/tools/local_core_runtime.sh"
CORE_RUNTIME="${ZLINK_LOCAL_CORE_RUNTIME}"
zlink_export_local_core_runtime
cd "$ROOT_DIR"

for arg in "$@"; do
  case "${arg}" in
    -h|--help)
      echo "Note: --core-version VERSION downloads and uses a released Core runtime."
      echo "      Without it, the current workspace Core (core/build) is used by default."
      echo "      --part-count N selects 1 or 2 application frames (default: 2)."
      ;;
  esac
done

NODE_VERSION="$(node -p 'process.versions.node')"
NODE_MAJOR="${NODE_VERSION%%.*}"
if [ "$NODE_MAJOR" -lt 22 ]; then
  echo "Error: Node perf requires Node 22 or newer; found ${NODE_VERSION}." >&2
  exit 1
fi

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
elif [ ! -f "$ROOT_DIR/dist-tools/perf/single/run_benchmarks.js" ]; then
  echo "Error: --reuse-build requested but dist-tools output is missing." >&2
  exit 1
fi

if [ ! -f "$CORE_RUNTIME" ]; then
  echo "Error: core runtime is missing: $CORE_RUNTIME" >&2
  echo "Build it first with: cmake --build core/build" >&2
  exit 1
fi

if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 0 ]] && find "$REPO_ROOT/core/include" "$REPO_ROOT/core/src" -type f -newer "$CORE_RUNTIME" | grep -q .; then
  echo "Error: core runtime is older than core/include or core/src." >&2
  echo "Rebuild it first with: cmake --build core/build" >&2
  echo "Runtime checked: $CORE_RUNTIME" >&2
  exit 1
fi

echo "Perf runtime libzlink: $(realpath "$CORE_RUNTIME")"
echo "Perf Node runtime: ${NODE_VERSION}"
export ZLINK_PERF_RUNTIME_LIBZLINK
ZLINK_PERF_RUNTIME_LIBZLINK="$(realpath "$CORE_RUNTIME")"
export PERF_CORE_SOURCE="${ZLINK_CORE_SOURCE}"
export PERF_CORE_VERSION="${ZLINK_CORE_VERSION}"
export PERF_CORE_RUNTIME="${ZLINK_PERF_RUNTIME_LIBZLINK}"

node dist-tools/perf/single/run_benchmarks.js "$@"
