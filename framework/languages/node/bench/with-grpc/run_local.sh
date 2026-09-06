#!/usr/bin/env bash
# with-grpc local bench runner, node row.
#
# spec section 3: one client process and one server process per implementation, loopback
# only, on the node port band of section 9 (5081-5089). Plan section 3.2: the measured span is
# serialized under /tmp/zlink-perf.lock and each run starts only under loadavg 2.0.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"

RUNS="${RUNS:-3}"
DURATION="${DURATION:-5}"
WARMUP="${WARMUP:-1000}"
PAYLOADS="${PAYLOADS:-1024,4096}"
WINDOW="${WINDOW:-100}"
STAMP="${STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUTROOT="${OUTROOT:-$HERE/log/$STAMP}"
TIMELINE="$OUTROOT/timeline.txt"

mkdir -p "$OUTROOT"

note() { echo "$(date --iso-8601=seconds) $*" | tee -a "$TIMELINE"; }

# spec section 9: if the band is occupied the runner stops. It does not move to another
# port, because then the recorded endpoint would not be the endpoint used.
for port in 5081 5082 5083 5084 5085 5086 5087; do
  if ss -ltn "( sport = :$port )" 2>/dev/null | grep -q LISTEN; then
    echo "port $port is in use; refusing to start (spec section 9)" >&2
    exit 1
  fi
done

start_servers() {
  node grpc-server/main.js --url 127.0.0.1:5081 --metrics-url http://127.0.0.1:5084 \
    > "$1/grpc-server.log" 2>&1 &
  node zlink-raw-server/main.js --endpoint tcp://127.0.0.1:5085 \
    --command-endpoint tcp://127.0.0.1:5087 --metrics-url http://127.0.0.1:5086 \
    > "$1/raw-server.log" 2>&1 &
  node zlink-framework-server/main.js --endpoint tcp://127.0.0.1:5082 \
    --metrics-url http://127.0.0.1:5083 > "$1/framework-server.log" 2>&1 &
  sleep 6
}

stop_servers() {
  for name in grpc-server zlink-raw-server zlink-framework-server; do
    pkill -f "^node ${name}/main.js" 2>/dev/null || true
  done
  sleep 1
}

gate_loadavg() {
  local reading
  reading="$(cut -d' ' -f1 /proc/loadavg)"
  note "loadavg gate for $1: $reading"
  if awk "BEGIN{exit !($reading >= 2.0)}"; then
    note "loadavg $reading >= 2.0; waiting"
    for _ in $(seq 1 60); do
      sleep 5
      reading="$(cut -d' ' -f1 /proc/loadavg)"
      awk "BEGIN{exit !($reading < 2.0)}" && break
    done
    note "loadavg after wait for $1: $reading"
  fi
}

one_run() {
  local label="$1" socket="$2"
  local dir="$OUTROOT/$label"
  mkdir -p "$dir"
  gate_loadavg "$label"
  note "run $label start (raw-socket=$socket)"
  start_servers "$dir"
  set +e
  node client/main.js \
    --payload-sizes "$PAYLOADS" --duration-seconds "$DURATION" --warmup "$WARMUP" \
    --request-window "$WINDOW" --raw-socket "$socket" \
    --grpc-url 127.0.0.1:5081 --grpc-stats-url http://127.0.0.1:5084 \
    --zlink-endpoint tcp://127.0.0.1:5082 --zlink-stats-url http://127.0.0.1:5083 \
    --zlink-raw-endpoint tcp://127.0.0.1:5085 --zlink-raw-stats-url http://127.0.0.1:5086 \
    --zlink-raw-command-endpoint tcp://127.0.0.1:5087 \
    --output "$dir" > "$dir/stdout.txt" 2> "$dir/stderr.txt"
  local rc=$?
  set -e
  stop_servers
  note "run $label end rc=$rc"
}

note "measured span begin: node=$(node --version) commit=$(git rev-parse --short HEAD)"
for i in $(seq 1 "$RUNS"); do
  one_run "node-router-$i" router
done
one_run "node-dealer-1" dealer
note "measured span end"
