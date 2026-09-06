#!/usr/bin/env bash
# with-grpc local bench runner, kotlin row.
#
# spec section 3: one client process and one server process per implementation, loopback
# only, on the kotlin port band of section 9 (5101-5109). Plan section 3.2: the measured
# span is serialized under /tmp/zlink-perf.lock and each run starts only under loadavg 2.0.
#
# The kotlin row owns the CLIENT. The three server processes are the java row's binaries,
# started here on the kotlin band: gRPC is language-neutral on the wire, the raw server is
# a ROUTER echo, and the framework server is the same zlink-framework-core host. What
# separates the kotlin row from the java row is the client-facing API, which is the
# comparison this phase is for.
#
# G7: the Gradle build runs BEFORE the measured span and under the repository's JVM build
# lock, so no compilation can land inside a measured window.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../../../.." && pwd)"
cd "$HERE"

RUNS="${RUNS:-3}"
# The DEALER comparison run is optional so that the three ROUTER runs -- the ones a
# judgement needs -- are taken first.
RUN_DEALER="${RUN_DEALER:-1}"
DURATION="${DURATION:-5}"
WARMUP_SECONDS="${WARMUP_SECONDS:-20}"
WARMUP_SEGMENT_SECONDS="${WARMUP_SEGMENT_SECONDS:-2}"
PAYLOADS="${PAYLOADS:-1024,4096}"
WINDOW="${WINDOW:-100}"
SKIP_BUILD="${SKIP_BUILD:-0}"
STAMP="${STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUTROOT="${OUTROOT:-$HERE/log/$STAMP}"
TIMELINE="$OUTROOT/timeline.txt"

export ZLINK_LIBRARY_PATH="${ZLINK_LIBRARY_PATH:-$REPO/.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so}"

mkdir -p "$OUTROOT"
note() { echo "$(date --iso-8601=seconds) $*" | tee -a "$TIMELINE"; }

# spec section 9: if the band is occupied the runner stops. It does not move to another
# port, because then the recorded endpoint would not be the endpoint used.
for port in 5101 5102 5103 5104 5105 5106 5107; do
  if ss -ltn "( sport = :$port )" 2>/dev/null | grep -q LISTEN; then
    echo "port $port is in use; refusing to start (spec section 9)" >&2
    exit 1
  fi
done

if [[ "$SKIP_BUILD" != "1" ]]; then
  note "build begin (outside the measured span, under /tmp/zlink-jvm-gate.lock)"
  flock --exclusive --timeout 1800 /tmp/zlink-jvm-gate.lock \
    "$REPO/framework/languages/java/gradlew" -p "$HERE" --no-daemon -q installDist
  note "build end"
fi

GRPC_BIN="$HERE/grpc-server/build/install/bench-grpc-server/bin/bench-grpc-server"
RAW_BIN="$HERE/zlink-raw-server/build/install/bench-zlink-raw-server/bin/bench-zlink-raw-server"
FW_BIN="$HERE/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server"
CLIENT_BIN="$HERE/kotlin-client/build/install/bench-kotlin-client/bin/bench-kotlin-client"
for binary in "$GRPC_BIN" "$RAW_BIN" "$FW_BIN" "$CLIENT_BIN"; do
  [[ -x "$binary" ]] || { echo "missing $binary; run without SKIP_BUILD=1" >&2; exit 1; }
done

SERVER_PIDS=()

start_servers() {
  local dir="$1"
  "$GRPC_BIN" --port 5101 --metrics-url http://127.0.0.1:5104 \
    > "$dir/grpc-server.log" 2>&1 &
  SERVER_PIDS+=($!)
  "$RAW_BIN" --endpoint tcp://127.0.0.1:5105 --command-endpoint tcp://127.0.0.1:5107 \
    --metrics-url http://127.0.0.1:5106 > "$dir/raw-server.log" 2>&1 &
  SERVER_PIDS+=($!)
  "$FW_BIN" --endpoint tcp://127.0.0.1:5102 --metrics-url http://127.0.0.1:5103 \
    > "$dir/framework-server.log" 2>&1 &
  SERVER_PIDS+=($!)
  for attempt in $(seq 1 120); do
    if curl -fsS http://127.0.0.1:5104/ready >/dev/null 2>&1 \
      && curl -fsS http://127.0.0.1:5106/ready >/dev/null 2>&1 \
      && curl -fsS http://127.0.0.1:5103/ready >/dev/null 2>&1; then
      note "servers ready after ${attempt}s"
      return 0
    fi
    sleep 1
  done
  note "servers did not become ready"
  return 1
}

stop_servers() {
  for pid in "${SERVER_PIDS[@]:-}"; do
    [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true
  done
  for pid in "${SERVER_PIDS[@]:-}"; do
    [[ -n "$pid" ]] && wait "$pid" 2>/dev/null || true
  done
  SERVER_PIDS=()
  sleep 2
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
  start_servers "$dir" || { stop_servers; note "run $label aborted: servers not ready"; return 0; }
  set +e
  "$CLIENT_BIN" \
    --payload-sizes "$PAYLOADS" --duration-seconds "$DURATION" \
    --warmup-seconds "$WARMUP_SECONDS" \
    --warmup-segment-seconds "$WARMUP_SEGMENT_SECONDS" \
    --request-window "$WINDOW" --raw-socket "$socket" \
    --grpc-url 127.0.0.1:5101 --grpc-stats-url http://127.0.0.1:5104 \
    --zlink-endpoint tcp://127.0.0.1:5102 --zlink-stats-url http://127.0.0.1:5103 \
    --zlink-raw-endpoint tcp://127.0.0.1:5105 --zlink-raw-stats-url http://127.0.0.1:5106 \
    --zlink-raw-command-endpoint tcp://127.0.0.1:5107 \
    --report-file with_grpc_kotlin.txt \
    --output "$dir" > "$dir/stdout.txt" 2> "$dir/stderr.txt"
  local rc=$?
  set -e
  stop_servers
  note "run $label end rc=$rc"
}

note "measured span begin: java=$(java -version 2>&1 | head -1) commit=$(git -C "$REPO" rev-parse --short HEAD)"
note "loadavg at span begin: $(cat /proc/loadavg)"
for i in $(seq 1 "$RUNS"); do
  one_run "kotlin-router-$i" router
done
if [[ "$RUN_DEALER" == "1" ]]; then
  one_run "kotlin-dealer-1" dealer
fi
note "loadavg at span end: $(cat /proc/loadavg)"
note "measured span end"
