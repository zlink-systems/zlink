#!/usr/bin/env bash
# Build every with-grpc bench (C, C++, .NET, Node, Java) without running a measurement.
# The local framework gate calls this so a runtime/API change that breaks a bench is caught
# before a measurement window (plan S6). Each language uses the same build commands as its
# runner; runners are then invoked with SKIP_BUILD=1 (C, .NET, Node, Java) or BUILD_DIR (C++).
#
# Inputs (all optional):
#   ZLINK_CORE_PACKAGE_PREFIX   Core prefix for the C bench (default: core/build)
#   ZLINK_LOCAL_PACKAGE_ROOT    local package root for C++/.NET/Java (default: .artifacts/wsl)
#   BENCH_LANGS                 space-separated subset of: c cpp dotnet node java (default: all)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LANGS="${BENCH_LANGS:-c cpp dotnet node java}"

require_quiet() {
  local load; load="$(cut -d' ' -f1 /proc/loadavg)"
  awk -v l="${load}" 'BEGIN { exit !(l < 10.0) }' || {
    echo "load average must be below 10 before build (current ${load})" >&2
    return 1
  }
}

build_c() {
  local build_dir="${HERE}/c/build"
  cmake -S "${HERE}/c" -B "${build_dir}" \
    -DZLINK_C_CORE_BUILD_DIR="${ZLINK_CORE_PACKAGE_PREFIX:-${HERE}/../../../core/build}"
  cmake --build "${build_dir}" --target \
    bench_c_with_grpc_zlink_server bench_c_with_grpc_zlink_client \
    bench_c_with_grpc_grpc_server bench_c_with_grpc_grpc_client --parallel 2
}

build_cpp() {
  cmake -S "${HERE}/cpp" -B "${HERE}/cpp/build" -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build "${HERE}/cpp/build" --parallel 2
}

build_dotnet() {
  dotnet build "${HERE}/dotnet/WithGrpcBench.sln" -c Release
}

build_node() {
  (cd "${HERE}/node" && npm ci --no-audit --no-fund && npm run build)
}

build_java() {
  (cd "${HERE}/java" && ./gradlew --no-daemon --max-workers=1 assemble installDist)
}

for lang in ${LANGS}; do
  case "${lang}" in
    c|cpp|dotnet|node|java) ;;
    *) echo "unknown bench language: ${lang}" >&2; exit 2 ;;
  esac
  require_quiet
  echo "[bench build] ${lang} start $(date +%H:%M:%S)"
  "build_${lang}"
  echo "[bench build] ${lang} ok $(date +%H:%M:%S)"
done
echo "BENCH_BUILD_ALL_OK"
