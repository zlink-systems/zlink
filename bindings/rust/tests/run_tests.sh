#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$PROJECT_DIR/../.." && pwd)"
CORE_LIB_DIR="$REPO_ROOT/core/build/lib"

cd "$PROJECT_DIR"

echo "=== zlink Rust binding tests ==="
echo ""

source "$HOME/.cargo/env" 2>/dev/null || true
RUST_NATIVE_DIR="${ZLINK_RUST_NATIVE_DIR:-$CORE_LIB_DIR}"
RUST_PACKAGE_VERSION="$(sed -n 's/^version = "\([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\)"/\1/p' "$PROJECT_DIR/Cargo.toml" | head -n1)"
RUST_RUNTIME="$RUST_NATIVE_DIR/libzlink.so"
[[ -f "$RUST_RUNTIME" ]] || RUST_RUNTIME="$RUST_NATIVE_DIR/libzlink.so.$RUST_PACKAGE_VERSION"
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

run_test_file() {
    local name="$1"
    local log_file
    log_file="$(mktemp)"
    TOTAL=$((TOTAL + 1))
    echo -n "  $name ... "
    if cargo test --test "$name" -- --test-threads=1 >"$log_file" 2>&1; then
        echo "PASS"
        PASS=$((PASS + 1))
    else
        echo "FAIL"
        cat "$log_file"
        FAIL=$((FAIL + 1))
    fi
    rm -f "$log_file"
}

echo "Running test suites:"
echo ""

run_test_file surface_tests
run_test_file contract_tests
run_test_file behavior_tests
run_test_file send_failure_tests
run_test_file receive_failure_tests
run_test_file boundary_tests
run_test_file option_tests
run_test_file ownership_tests
run_test_file monitor_tests
run_test_file optimization_guard_tests

TOTAL=$((TOTAL + 1))
echo -n "  samples ... "
if "$PROJECT_DIR/samples/run_samples.sh" >/tmp/zlink-rust-samples.log 2>&1; then
    echo "PASS"
    PASS=$((PASS + 1))
else
    echo "FAIL"
    cat /tmp/zlink-rust-samples.log
    FAIL=$((FAIL + 1))
fi
rm -f /tmp/zlink-rust-samples.log

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
