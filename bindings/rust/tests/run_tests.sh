#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$PROJECT_DIR/../.." && pwd)"
source "${REPO_ROOT}/bindings/tools/local_core_runtime.sh"
if [[ "${ZLINK_CORE_RELEASE_MODE}" -eq 1 ]]; then
    CORE_LIB_DIR="${ZLINK_CORE_PACKAGE_PREFIX}/lib"
else
    CORE_LIB_DIR="$REPO_ROOT/core/build/lib"
fi

cd "$PROJECT_DIR"

echo "=== zlink Rust binding tests ==="
echo ""

source "$HOME/.cargo/env" 2>/dev/null || true
RUST_NATIVE_DIR="${ZLINK_RUST_NATIVE_DIR:-$CORE_LIB_DIR}"
RUST_CORE_VERSION="$(sed -n 's/^LIBZLINK_VERSION=//p' "$REPO_ROOT/VERSION")"
RUST_RUNTIME="$RUST_NATIVE_DIR/libzlink.so"
[[ -f "$RUST_RUNTIME" ]] || RUST_RUNTIME="$RUST_NATIVE_DIR/libzlink.so.$RUST_CORE_VERSION"
[[ -f "$RUST_RUNTIME" ]] || {
    echo "Rust test runtime not found: $RUST_NATIVE_DIR" >&2
    echo "Build core/build or set ZLINK_RUST_NATIVE_DIR." >&2
    exit 1
}
RUST_RUNTIME="$(readlink -f "$RUST_RUNTIME" 2>/dev/null || printf '%s' "$RUST_RUNTIME")"
export ZLINK_RUST_NATIVE_DIR="$RUST_NATIVE_DIR"
export LD_LIBRARY_PATH="$RUST_NATIVE_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-/tmp/zlink-rust-target/tests}"

echo "Rust test runtime: $RUST_RUNTIME"
echo "Rust test runtime sha256: $(sha256sum "$RUST_RUNTIME" | awk '{print $1}')"

PASS=0
FAIL=0
TOTAL=0

run_check() {
    local name="$1"
    shift
    local log_file
    log_file="$(mktemp)"
    TOTAL=$((TOTAL + 1))
    echo -n "  $name ... "
    if "$@" >"$log_file" 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        cat "$log_file"
        FAIL=$((FAIL + 1))
    fi
    rm -f "$log_file"
}

run_test_file() {
    local name="$1"
    shift
    run_check "$name" cargo test --test "$name" -- --test-threads=1 "$@"
}

echo "Running test suites:"
echo ""

run_check perf_builds bash "$SCRIPT_DIR/build_perf.sh"
run_check lib_tests cargo test --lib -- --test-threads=1

run_test_file surface_tests
run_test_file single_part_or_throw_tests
run_test_file contract_tests
run_test_file router_selected_route_tests
run_test_file ffi_layout_tests
run_test_file behavior_tests
run_test_file send_failure_tests
run_test_file receive_failure_tests
run_test_file routed_async_tests
run_test_file boundary_tests
run_test_file option_tests
run_test_file optional_socket_duration_tests
run_test_file subscription_at_tests
run_test_file flow_state_tests
run_test_file ownership_tests
run_test_file monitor_tests
run_test_file context_poller_close_tests
run_test_file connection_attempt_tests

run_test_file closed_submission_tests
run_test_file missing_received_context_tests
run_test_file duration_boundary_tests
run_test_file endpoint_boundary_tests
run_test_file envelope_state_tests
run_test_file peer_weight_boundary_tests
run_test_file routed_option_getter_tests
run_test_file typed_monitor_tests
run_test_file tls_tests
run_test_file timer_wait_tests
run_test_file raw_poll_tests
run_test_file multipart_close_tests
run_test_file proxy_tests
run_test_file error_catalog_tests
run_test_file internal_errno_tests
run_test_file socket_thread_traits_tests
run_test_file raw_reply_backpressure_tests
run_test_file router_move_reply_tests
run_test_file stream_recv_mode_boundary_tests

run_ownership_guard() {
    local shim="$1" suite="$2" test="$3" marker="$4" log_file
    log_file="$(mktemp)"
    TOTAL=$((TOTAL + 1))
    echo -n "  $test ... "
    if LD_PRELOAD="$shim${LD_PRELOAD:+:$LD_PRELOAD}" ZLINK_AUDIT_MARKER="$marker" \
        cargo test --test "$suite" -- --ignored --exact "$test" --test-threads=1 \
            >"$log_file" 2>&1 && [[ -s "$marker" ]]; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        cat "$log_file"
        FAIL=$((FAIL + 1))
    fi
    rm -f "$log_file"
}

if [[ "$(uname -s)" == Linux ]]; then
    GUARD_DIR="$(mktemp -d)"
    trap 'rm -rf "$GUARD_DIR"' EXIT
    SEND_SHIM="$GUARD_DIR/send_no_wait_token_shim.so"
    if "${CC:-cc}" -shared -fPIC -I"$PROJECT_DIR/include" -o "$SEND_SHIM" \
        "$SCRIPT_DIR/test_support/send_no_wait_token_shim.c"; then
        LD_PRELOAD="$SEND_SHIM${LD_PRELOAD:+:$LD_PRELOAD}" \
            run_test_file send_without_wait_token_tests \
                --ignored --exact send_without_wait_token_returns_backpressure
    else
        echo "  SEND without wait token shim ... FAIL"
        TOTAL=$((TOTAL + 1))
        FAIL=$((FAIL + 1))
    fi
    GUARD_SHIM="$GUARD_DIR/message_ownership_shim.so"
    if "${CC:-cc}" -shared -fPIC -I"$PROJECT_DIR/include" -o "$GUARD_SHIM" \
        "$SCRIPT_DIR/test_support/message_ownership_shim.c" -ldl; then
        run_ownership_guard "$GUARD_SHIM" ownership_tests \
            multipart_recv_adopts_parts_into_uninitialized_message_slots "$GUARD_DIR/adopt"
        run_ownership_guard "$GUARD_SHIM" ownership_tests \
            blocking_send_and_reply_do_not_copy_message_parts "$GUARD_DIR/copy"
        run_ownership_guard "$GUARD_SHIM" behavior_tests \
            stream_packet_receive_propagates_message_init_failure "$GUARD_DIR/init"
    else
        echo "  message ownership shim ... FAIL"
        TOTAL=$((TOTAL + 1))
        FAIL=$((FAIL + 1))
    fi
fi

run_check samples "$PROJECT_DIR/samples/run_samples.sh"

echo ""
echo "=== Summary ==="
echo "  Total:  $TOTAL"
echo "  Pass:   $PASS"
echo "  Fail:   $FAIL"
echo ""

if [ "$FAIL" -gt 0 ]; then
    echo "RESULT: FAIL"
    exit 1
else
    echo "RESULT: PASS"
    exit 0
fi
