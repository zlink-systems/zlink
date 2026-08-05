#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${ZLINK_CPP_BUILD_DIR:-${script_dir}/../../build}"

"${build_dir}/connector_perf_client" "$@"
