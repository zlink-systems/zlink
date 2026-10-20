#!/usr/bin/env bash
# Membership §4 completion-owner parity. Each language exercises its own
# source/target lifecycle; transport interoperability remains in the smoke suite.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${script_dir}/../../../.." && pwd)"
cpp_build="${ZLINK_CPP_BUILD_DIR:-${script_dir}/../build/linux-ninja-release}"
log_dir="${ZLINK_JOIN_COMPLETION_LOG_DIR:-${repo_root}/.artifacts/join-completion-owner}"
mkdir -p "${log_dir}"

# The pre-ready failure belongs to the source. After relay readiness, source
# authority loss releases retained work and cannot add a public completion.
dotnet test "${repo_root}/framework/languages/dotnet/tests/Zlink.Framework.UnitTests" \
  --filter 'FullyQualifiedName~JoinCallbackSendTests|FullyQualifiedName~RelocationBehaviorConformanceTests' \
  >"${log_dir}/dotnet.log" 2>&1
(
  cd "${repo_root}/framework/languages/java"
  ./gradlew --no-daemon :zlink-framework-core:test \
    --tests '*ZLinkCanonicalDirectJoinHostIntegrationTest' \
    --tests '*ZLinkCommittedActorLocalIngressTest' \
    --tests '*ZLinkRelocationHandOffTest'
) >"${log_dir}/java.log" 2>&1
(
  cd "${repo_root}/framework/languages/node"
  npm run build
  node node_modules/typescript/bin/tsc -p tsconfig.m6c-runtime.json
  node --test test/contract/deferred-actor-join.test.js
  node --test build/m6c-runtime/languages/node/test/m6c/m6c-host-relocation.contract.js
) >"${log_dir}/node.log" 2>&1
while true; do
  raw_cmake_count="$(ps -ef 2>/dev/null | grep -c '[c]make --build')" || [[ $? -eq 1 ]]
  # A shell wrapper can contain the same command text; count actual builders.
  active_cmake_count="$(ps -eo comm=,args= 2>/dev/null | awk '$1 == "cmake" && /--build/ { count++ } END { print count + 0 }')"
  if (( active_cmake_count < 2 )); then
    printf 'raw grep count=%s, actual cmake build processes=%s\n' \
      "${raw_cmake_count}" "${active_cmake_count}" >"${log_dir}/cpp-build-count.log"
    break
  fi
  sleep 5
done
cmake --build "${cpp_build}" --target test_cpp_framework_spot_close_conformance --parallel 4 \
  >"${log_dir}/cpp-build.log" 2>&1
GTEST_FILTER='ActorMaintenanceJoin.RelayedSourceCleanupFailureDoesNotFailDeferredJoin:ActorMaintenanceJoin.MissingAfterTargetCommitAndDestroyDoesNotCompleteOnSource:*ActorDeferredJoinFailure.FailureAndRejectionPreserveSourceActor/*' \
  ctest --test-dir "${cpp_build}" -R '^test_cpp_framework_spot_close_conformance$' \
    --output-on-failure >"${log_dir}/cpp.log" 2>&1
printf '%s\n' 'Join completion owner contract: dotnet/java/node/cpp passed'
