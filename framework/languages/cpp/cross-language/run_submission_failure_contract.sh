#!/usr/bin/env bash
# Error model §§3/5: unclassified pre-submit failures are InternalFailure with
# the original cause; cancellation, caller/configuration and typed errors retain
# their meaning. Each language runs its actual resolver/transport boundary.
# Usage: run_submission_failure_contract.sh <configured-cpp-build-dir> <log-dir>
# Until #2351 merges, ZLINK_SUBMISSION_NODE_ROOT selects its built Node checkout.
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
CPP_BUILD=$(realpath "${1:?configured C++ build directory required}")
LOG_DIR=$(realpath -m "${2:?log directory required}")
mkdir -p "$LOG_DIR"
failed=0

run_contract() {
    local language=$1
    shift
    if "$@" >"$LOG_DIR/$language.log" 2>&1; then
        printf '%s: PASS\n' "$language"
    else
        printf '%s: FAIL (%s/%s.log)\n' "$language" "$LOG_DIR" "$language"
        failed=1
    fi
}

dotnet_contract() {
    rm -f "$LOG_DIR/submission-dotnet.trx"
    dotnet test \
        "$ROOT/framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj" \
        -m:2 --filter 'FullyQualifiedName~RequestSubmissionFailureContract' \
        --logger 'trx;LogFileName=submission-dotnet.trx' --results-directory "$LOG_DIR" || return
    # dotnet test can exit successfully with a filter that matches no tests.
    python3 - "$LOG_DIR/submission-dotnet.trx" <<'PY'
import sys
import xml.etree.ElementTree as ET

root = ET.parse(sys.argv[1]).getroot()
counters = root.find('.//{*}Counters')
if counters is None:
    raise SystemExit('missing test counters')
if int(counters.attrib['total']) == 0:
    raise SystemExit('no submission contract tests ran')
if counters.attrib['passed'] != counters.attrib['total']:
    raise SystemExit(str(counters.attrib))
PY
}

java_contract() (
    cd "$ROOT/framework/languages/java" || return
    ./gradlew --no-daemon --max-workers=2 :zlink-framework-core:test --rerun \
        --tests '*ZLinkChannelRuntimeTest.meshSubmissionUnexpectedFailureContract' \
        --tests '*ZLinkChannelRuntimeTest.meshSubmissionKnownFailureContract'
)

run_contract dotnet dotnet_contract
run_contract java java_contract
run_contract node node --test "$ROOT/framework/languages/cpp/cross-language/submission_failure_contract.cjs"
run_contract cpp ctest --test-dir "$CPP_BUILD" --verbose --output-on-failure --no-tests=error \
    -R '^test_cpp_framework_submission_failure_contract$'
exit "$failed"
