#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/dotnet-env.sh"
exec python3 "${SCRIPT_DIR}/../../../../perf/runner/environment.py" --language dotnet --perf-dir "${SCRIPT_DIR}/.." "$@"
