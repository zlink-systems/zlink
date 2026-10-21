#!/usr/bin/env bash
# Transport liveness §3 and binding connect results: native absence completes
# cleanup; other native failures remain visible to the caller.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../../.." && pwd)"
cpp_build="${ZLINK_CPP_BUILD_DIR:-${script_dir}/../build/linux-ninja-release}"
log_dir="${ZLINK_LIVENESS_CONTRACT_LOG_DIR:-${repo_root}/.artifacts/liveness-disconnect-contract}"
mkdir -p "${log_dir}"

dotnet test "${repo_root}/framework/languages/dotnet/tests/Zlink.Framework.UnitTests" \
  -m:2 -nodeReuse:false -p:UseSharedCompilation=false \
  --filter 'FullyQualifiedName~LivenessDisconnect_' \
  >"${log_dir}/dotnet.log" 2>&1
(
  cd "${repo_root}/framework/languages/java"
  ./gradlew --no-daemon --max-workers=2 :zlink-framework-core:test \
    --tests '*ZLinkJavaLivenessNativeContractTest' --rerun-tasks
) >"${log_dir}/java.log" 2>&1
(
  cd "${repo_root}/framework/languages/node"
  npm run build
  node --test test/contract/liveness-disconnect-result.test.js
) >"${log_dir}/node.log" 2>&1
cmake --build "${cpp_build}" --target test_cpp_framework_m6a_runtime --parallel 4 \
  >"${log_dir}/cpp-build.log" 2>&1
ctest --test-dir "${cpp_build}" -R '^test_cpp_framework_liveness_not_found_contract$' \
  --no-tests=error --output-on-failure >"${log_dir}/cpp.log" 2>&1
printf '%s\n' 'Liveness disconnect result contract: dotnet/java/node/cpp passed'
