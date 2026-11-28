#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TEST_BUILD_DIR="${1:-$(cd "${SCRIPT_DIR}/.." && pwd)/build}"

cmake() {
  if [[ "$1" == -S && "$3" == -B ]]; then
    printf 'configured-build-dir=%s\n' "$4"
  fi
}

ctest() { :; }
export -f cmake ctest

output="$(ZLINK_CORE_SOURCE=local bash "${SCRIPT_DIR}/../samples/run_samples.sh")"
sample_build_dir="$(sed -n 's/^configured-build-dir=//p' <<< "$output")"
if [[ -z "$sample_build_dir" || "$sample_build_dir" == "$TEST_BUILD_DIR" ]]; then
  printf 'Expected a separate sample build directory. Samples: %s. Tests: %s.\n' \
    "$sample_build_dir" "$TEST_BUILD_DIR" >&2
  exit 1
fi

echo '[cpp-tests] sample build directory isolation passed'
