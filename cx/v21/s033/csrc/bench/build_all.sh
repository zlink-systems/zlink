#!/usr/bin/env bash
# Builds every with-grpc bench without running a measurement, by calling each language's build.sh
# (the one owner of that language's build command; the runners call the same file).
# The local framework gate calls this so a runtime/API change that breaks a bench is caught
# before a measurement window (plan S6).
#
# Inputs (all optional):
#   BENCH_LANGS   space-separated subset of: c cpp dotnet java node (default: all).
#                 Kotlin has no entry: java/build.sh builds the Kotlin bench in the same Gradle build.
#   Each build.sh reads its own inputs, e.g. ZLINK_CORE_PACKAGE_PREFIX and ZLINK_LOCAL_PACKAGE_ROOT
#   (c/build.sh), VCPKG_ROOT (cpp/build.sh).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LANGS="${BENCH_LANGS:-c cpp dotnet java node}"

for lang in ${LANGS}; do
  case "${lang}" in
    c|cpp|dotnet|java|node) ;;
    *) echo "unknown bench language: ${lang}" >&2; exit 2 ;;
  esac
  echo "[bench build] ${lang} start $(date +%H:%M:%S)"
  "${HERE}/${lang}/build.sh"
  echo "[bench build] ${lang} ok $(date +%H:%M:%S)"
done
echo "BENCH_BUILD_ALL_OK"
