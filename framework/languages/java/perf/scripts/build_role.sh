#!/usr/bin/env bash
# Build one Java or Kotlin perf role. Maps the common runner's build inputs to the Gradle properties of the shared
# sample settings (perf/README.ko.md §17.2); perf/settings.gradle.kts validates the inputs themselves.
set -euo pipefail
if [[ $# -ne 2 ]]; then
  echo "Usage: build_role.sh <gradle project dir> <gradle task>" >&2
  exit 2
fi
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
package_mode=false
if [[ "${ZLINK_PERF_PACKAGE_SOURCE:-}" == published ]]; then
  package_mode=true
fi
exec "${SCRIPT_DIR}/../../gradlew" --project-dir "$1" "$2" -q \
  "-Pzlink.frameworkVersion=${ZLINK_PERF_FRAMEWORK_VERSION:-}" "-Pzlink.samples.packageMode=${package_mode}"
