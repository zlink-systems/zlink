#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
show_help=0
for arg in "$@"; do
    case "$arg" in
        -h|--help)
            show_help=1
            ;;
    esac
done

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

exec python -u "$ROOT_DIR/run_benchmarks.py" "$@"
