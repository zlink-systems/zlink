#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$ROOT_DIR/../../../.." && pwd)"
show_help=0
for arg in "$@"; do
    case "$arg" in
        -h|--help)
            show_help=1
            ;;
    esac
done

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

# An explicit ZLINK_LIBRARY_PATH (Core or wheel runtime) always wins, exactly
# as before. Otherwise, use the current workspace Core by default; an explicit
# --core-version selects the downloaded release package for that version instead.
if [[ -n "${ZLINK_LIBRARY_PATH:-}" ]]; then
  if [[ -n "${CORE_VERSION_OPTION}" ]]; then
    echo "Error: --core-version cannot be combined with an explicit ZLINK_LIBRARY_PATH." >&2
    exit 1
  fi
else
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
  source "${REPO_DIR}/bindings/tools/local_core_runtime.sh"
  export ZLINK_LIBRARY_PATH="${ZLINK_LOCAL_CORE_RUNTIME}"
fi
export PERF_CORE_SOURCE="${ZLINK_CORE_SOURCE:-explicit}"
export PERF_CORE_VERSION="${ZLINK_CORE_VERSION:-$(awk -F= '/^LIBZLINK_VERSION=/{print $2}' "${REPO_DIR}/VERSION")}"
export PERF_CORE_RUNTIME="$(readlink -f "${ZLINK_LIBRARY_PATH}" 2>/dev/null || printf '%s' "${ZLINK_LIBRARY_PATH}")"

if [[ $show_help -eq 1 ]]; then
    echo "Note: --core-version VERSION downloads and uses a released Core runtime."
    echo "      Without it (and without an explicit ZLINK_LIBRARY_PATH), the current"
    echo "      workspace Core (core/build) is used by default."
    echo "      --part-count N selects 1 or 2 application frames (default: 2)."
fi

if [[ $show_help -eq 0 ]]; then
    if [[ -z "${ZLINK_LIBRARY_PATH:-}" ]]; then
        echo "Error: ZLINK_LIBRARY_PATH is required for Python perf" >&2
        echo "Pass the approved Core or wheel runtime explicitly." >&2
        exit 1
    fi
    if [[ ! -f "$ZLINK_LIBRARY_PATH" ]]; then
        echo "Error: Python perf runtime is missing: $ZLINK_LIBRARY_PATH" >&2
        exit 1
    fi
    export ZLINK_LIBRARY_PATH="$(realpath "$ZLINK_LIBRARY_PATH")"
    echo "Perf runtime libzlink: $ZLINK_LIBRARY_PATH"
    echo "Perf runtime sha256: $(sha256sum "$ZLINK_LIBRARY_PATH" | awk '{print $1}')"
fi

export PYTHONDONTWRITEBYTECODE="${PYTHONDONTWRITEBYTECODE:-1}"
PYTHON_EXECUTABLE="${PYTHON_EXECUTABLE:-python3}"

exec "$PYTHON_EXECUTABLE" -u "$ROOT_DIR/run_benchmarks.py" "$@"
