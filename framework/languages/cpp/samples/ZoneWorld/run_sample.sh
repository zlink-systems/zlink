#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$CPP_ROOT/samples/sample-build-common.sh"
zlink_cpp_sample_prepare_build "$CPP_ROOT"

cmake --build "$BUILD_DIR" --parallel 2 --target sample_cpp_framework_zoneworld_zone_node \
  sample_cpp_framework_zoneworld_gateway sample_cpp_framework_zoneworld_ops \
  sample_cpp_framework_zoneworld_client >/dev/null

RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "$RUN_DIR")-$$-${RANDOM}"
LOG_DIR="$RUN_DIR/logs"
mkdir -p "$LOG_DIR"
PIDS=()
REDIS_CONTAINER_NAME=""
cleanup() {
  local code=$?
  if [[ "$code" -ne 0 ]]; then
    for log in "$LOG_DIR"/*.log; do
      [[ -f "$log" ]] || continue
      echo "===== $log" >&2
      sed -n '1,2400p' "$log" >&2
    done
  fi
  for ((i=${#PIDS[@]}-1; i>=0; i--)); do kill "${PIDS[$i]}" >/dev/null 2>&1 || true; done
  for _ in $(seq 1 30); do
    local any_running=false
    for pid in "${PIDS[@]}"; do kill -0 "$pid" >/dev/null 2>&1 && any_running=true; done
    [[ "$any_running" == true ]] || break
    sleep 0.1
  done
  for pid in "${PIDS[@]}"; do kill -9 "$pid" >/dev/null 2>&1 || true; done
  for pid in "${PIDS[@]}"; do wait "$pid" >/dev/null 2>&1 || true; done
  [[ -z "$REDIS_CONTAINER_NAME" ]] || docker rm -fv "$REDIS_CONTAINER_NAME" >/dev/null 2>&1 || true
  rm -rf "$RUN_DIR"
  exit "$code"
}
trap cleanup EXIT INT TERM

read -r NODE1_MESH NODE2_MESH GATEWAY_MESH OPS_MESH GAME_STREAM OPS_STREAM BROADCAST NODE1_HTTP NODE2_HTTP GATEWAY_HTTP <<<"$(python3 - <<'PY'
import socket
sockets=[]
for _ in range(10):
    s=socket.socket(); s.bind(('127.0.0.1',0)); sockets.append(s)
print(' '.join(f'tcp://127.0.0.1:{s.getsockname()[1]}' for s in sockets))
for s in sockets: s.close()
PY
)"
zlink_redis_start_scoped_assign REDIS_CONTAINER_NAME redis_port "zlink-redis-cpp-sample-zoneworld" "redis:7-alpine"
export ZONEWORLD_REDIS_ENDPOINT="tcp://127.0.0.1:${redis_port}"
export ZONEWORLD_REDIS_KEY_PREFIX="zoneworld:cpp:${RUN_ID}:"
export ZONEWORLD_BROADCAST_ENDPOINT="$BROADCAST"
export ZONEWORLD_LOG_DIR="$LOG_DIR"

start_role() { local label="$1"; shift; env "$@" >"$LOG_DIR/$label.log" 2>&1 & PIDS+=("$!"); }
wait_port() {
  local endpoint="${1#tcp://}"
  local port="${endpoint##*:}"
  for _ in $(seq 1 300); do
    if (echo >/dev/tcp/127.0.0.1/"$port") >/dev/null 2>&1; then return 0; fi
    sleep 0.1
  done
  for log in "$LOG_DIR"/*.log; do echo "===== $log" >&2; sed -n '1,2400p' "$log" >&2; done
  return 1
}

start_role zone-node-1 ZONEWORLD_NODE_ID=zone-node-1 ZONEWORLD_MESH_ENDPOINT="$NODE1_MESH" ZONEWORLD_BOOTSTRAP_HTTP_ENDPOINT="${NODE1_HTTP/tcp:/http:}" "$BIN_DIR/sample_cpp_framework_zoneworld_zone_node"
wait_port "$NODE1_MESH"
sleep 1
curl --max-time 20 -fsS -X POST -H 'content-type: application/json' -d '{}' "${NODE1_HTTP/tcp:/http:}/bootstrap" >/dev/null
start_role zone-node-2 ZONEWORLD_NODE_ID=zone-node-2 ZONEWORLD_MESH_ENDPOINT="$NODE2_MESH" ZONEWORLD_PEER_ENDPOINT="$NODE1_MESH" ZONEWORLD_BOOTSTRAP_HTTP_ENDPOINT="${NODE2_HTTP/tcp:/http:}" "$BIN_DIR/sample_cpp_framework_zoneworld_zone_node"
wait_port "$NODE2_MESH"
sleep 1
curl --max-time 20 -fsS -X POST -H 'content-type: application/json' -d '{}' "${NODE2_HTTP/tcp:/http:}/bootstrap" >/dev/null
start_role ops ZONEWORLD_MESH_ENDPOINT="$OPS_MESH" ZONEWORLD_PEER_ENDPOINT="$NODE1_MESH" ZONEWORLD_PEER_ENDPOINT_2="$NODE2_MESH" ZONEWORLD_STREAM_ENDPOINT="$OPS_STREAM" "$BIN_DIR/sample_cpp_framework_zoneworld_ops"
wait_port "$OPS_STREAM"
start_role gateway ZONEWORLD_MESH_ENDPOINT="$GATEWAY_MESH" ZONEWORLD_PEER_ENDPOINT="$NODE1_MESH" ZONEWORLD_PEER_ENDPOINT_2="$NODE2_MESH" ZONEWORLD_STREAM_ENDPOINT="$GAME_STREAM" ZONEWORLD_BOOTSTRAP_HTTP_ENDPOINT="${GATEWAY_HTTP/tcp:/http:}" "$BIN_DIR/sample_cpp_framework_zoneworld_gateway"
wait_port "$GAME_STREAM"

sleep 5
"$BIN_DIR/sample_cpp_framework_zoneworld_client" --game-endpoint "$GAME_STREAM" \
  --ops-endpoint "$OPS_STREAM" |& tee "$LOG_DIR/client.log"
grep -q '^zoneworld=completed$' "$LOG_DIR/client.log"
grep -q '^zoneworld-relocation=completed$' "$LOG_DIR/client.log"
grep -q '^zoneworld-border=completed$' "$LOG_DIR/client.log"
grep -q '^zoneworld-ops=completed$' "$LOG_DIR/client.log"
for scenario in ZW-A3 ZW-A5 ZW-B2 ZW-B6 ZW-B7 ZW-C1 ZW-D1; do
  grep -q "^scenario $scenario passed\$" "$LOG_DIR/client.log"
done
curl --max-time 30 -fsS -X POST -H 'content-type: application/json' -d '{}' "${GATEWAY_HTTP/tcp:/http:}/bootstrap-bots" >/dev/null
sleep 6
[[ "$(grep -h -c 'zoneworld-actor-joined.*player=bot-' "$LOG_DIR"/zone-node-*.log | awk '{ total += $1 } END { print total + 0 }')" -ge 8 ]]
grep -q 'zoneworld-bot-move player=bot-' "$LOG_DIR"/zone-node-*.log
grep -q 'zoneworld-border-received' "$LOG_DIR"/zone-node-*.log
grep -q 'zoneworld-zone-ready' "$LOG_DIR"/zone-node-*.log
echo "PASS ZoneWorld.Cpp"
echo "zoneworld sample result=passed"
