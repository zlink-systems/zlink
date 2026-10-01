#!/usr/bin/env bash
# Runs the same stale-route contract at each language's resolver/transport boundary.
# The wire smoke clients use requestToNode and cannot count resolver invalidations.
# Usage: run_spot_terminal_contract.sh <configured-cpp-build-dir> <log-dir>
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

# Common expectation: first terminal retained; resolve=1, invalidate=1,
# submit=1, resubmit=0, cold activation=0 for an existing stale owner.
run_contract cpp ctest --test-dir "$CPP_BUILD" --output-on-failure --no-tests=error \
    -R '^test_cpp_framework_channel_messaging$'
(
    cd "$ROOT/framework/languages/java" || exit 1
    ./gradlew --no-daemon :zlink-framework-core:test --rerun \
        --tests systems.zlink.framework.runtime.spots.ZLinkDefaultSpotOutboundTerminalTest
) >"$LOG_DIR/java.log" 2>&1
if (( $? == 0 )); then printf 'java: PASS\n'; else printf 'java: FAIL (%s/java.log)\n' "$LOG_DIR"; failed=1; fi
run_contract dotnet dotnet test \
    "$ROOT/framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj" \
    --filter 'FullyQualifiedName~Spot_Request_Preserves_First_Stale_Terminal_Without_Resubmit|FullyQualifiedName~Spot_Handle_Stale_Terminal_Is_Preserved_With_One_Invalidation|FullyQualifiedName~Spot_Request_Remote_Terminal_Invalidates_Once_Only_For_Framework_Origin'
(
    cd "$ROOT/framework/languages/node" || exit 1
    node --test --test-name-pattern='Instance Spot stale terminal' test/contract/object-routing.test.js
) >"$LOG_DIR/node.log" 2>&1
if (( $? == 0 )); then printf 'node: PASS\n'; else printf 'node: FAIL (%s/node.log)\n' "$LOG_DIR"; failed=1; fi
exit "$failed"
