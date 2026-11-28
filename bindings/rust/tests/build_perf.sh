#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

status=0
for suite in multi single; do
    echo "Building Rust perf $suite"
    if ! cargo build --release --all-targets --locked \
        --manifest-path "$PROJECT_DIR/perf/$suite/Cargo.toml"; then
        status=1
    fi
done
exit "$status"
