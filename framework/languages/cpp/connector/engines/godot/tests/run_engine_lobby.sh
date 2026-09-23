#!/usr/bin/env bash
set -euo pipefail

test_binary="$1"
server_dir="$2"
cd "$server_dir"
./run_sample.sh install
./run_sample.sh build
./run_sample.sh run
trap './run_sample.sh stop' EXIT
stream_port="$(cat .run/stream.port)"
"$test_binary" "ws://127.0.0.1:${stream_port}"
