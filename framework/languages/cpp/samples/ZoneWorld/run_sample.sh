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
  [[ -z "$REDIS_CONTAINER_NAME" ]] || zlink_redis_remove_by_id "$REDIS_CONTAINER_NAME" || true
  rm -rf "$RUN_DIR"
  exit "$code"
}
trap cleanup EXIT INT TERM

read -r -a ZONEWORLD_PORTS <<<"$(zlink_sample_allocate_ports 13)"
NODE1_MESH="tcp://127.0.0.1:${ZONEWORLD_PORTS[0]}"
NODE2_MESH="tcp://127.0.0.1:${ZONEWORLD_PORTS[1]}"
GATEWAY_MESH="tcp://127.0.0.1:${ZONEWORLD_PORTS[2]}"
OPS_MESH="tcp://127.0.0.1:${ZONEWORLD_PORTS[3]}"
GAME_STREAM="tcp://127.0.0.1:${ZONEWORLD_PORTS[4]}"
OPS_STREAM="tcp://127.0.0.1:${ZONEWORLD_PORTS[5]}"
BROADCAST="tcp://127.0.0.1:${ZONEWORLD_PORTS[6]}"
NODE1_HTTP="tcp://127.0.0.1:${ZONEWORLD_PORTS[7]}"
NODE2_HTTP="tcp://127.0.0.1:${ZONEWORLD_PORTS[8]}"
GATEWAY_HTTP="tcp://127.0.0.1:${ZONEWORLD_PORTS[9]}"
NODE1_STREAM="tcp://127.0.0.1:${ZONEWORLD_PORTS[10]}"
NODE2_STREAM="tcp://127.0.0.1:${ZONEWORLD_PORTS[11]}"
OPS_HTTP="tcp://127.0.0.1:${ZONEWORLD_PORTS[12]}"
zlink_redis_start_scoped_assign REDIS_CONTAINER_NAME redis_port "zlink-redis-cpp-sample-zoneworld" "redis:7-alpine"

CONFIG_DIR="$RUN_DIR/config"
mkdir -p "$CONFIG_DIR"
write_role_config() {
  local path="$1" node_id="$2" mesh_endpoint="$3" stream_endpoint="$4" http_endpoint="$5"
  python3 - "$path" "$node_id" "$mesh_endpoint" "$stream_endpoint" \
    "$http_endpoint" "tcp://127.0.0.1:${redis_port}" \
    "zoneworld:cpp:${RUN_ID}:" "$BROADCAST" "$LOG_DIR" <<'CONFIG_PY'
import json
import os
import stat
import sys

path, node_id, mesh_endpoint, stream_endpoint, http_endpoint, redis_endpoint, redis_key_prefix, broadcast_endpoint, log_dir = sys.argv[1:]
document = {
    "sample": {
        "zoneworld": {
            "redisEndpoint": redis_endpoint,
            "redisKeyPrefix": redis_key_prefix,
            "nodeId": node_id,
            "meshEndpoint": mesh_endpoint,
            "streamEndpoint": stream_endpoint,
            "broadcastEndpoint": broadcast_endpoint,
            "bootstrapHttpEndpoint": http_endpoint,
            "logDir": log_dir,
        }
    }
}
with open(path, "w", encoding="utf-8") as file:
    json.dump(document, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
CONFIG_PY
}

write_role_config "$CONFIG_DIR/zone-node-1.json" zone-node-1 "$NODE1_MESH" \
  "$NODE1_STREAM" "${NODE1_HTTP/tcp:/http:}"
write_role_config "$CONFIG_DIR/zone-node-2.json" zone-node-2 "$NODE2_MESH" \
  "$NODE2_STREAM" "${NODE2_HTTP/tcp:/http:}"
write_role_config "$CONFIG_DIR/ops.json" ops "$OPS_MESH" "$OPS_STREAM" \
  "${OPS_HTTP/tcp:/http:}"
write_role_config "$CONFIG_DIR/gateway.json" gateway "$GATEWAY_MESH" "$GAME_STREAM" \
  "${GATEWAY_HTTP/tcp:/http:}"

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

start_role zone-node-1 "$BIN_DIR/sample_cpp_framework_zoneworld_zone_node" \
  --config="$CONFIG_DIR/zone-node-1.json"
wait_port "$NODE1_MESH"
sleep 1
curl --max-time 20 -fsS -X POST -H 'content-type: application/json' -d '{}' "${NODE1_HTTP/tcp:/http:}/bootstrap" >/dev/null
start_role zone-node-2 "$BIN_DIR/sample_cpp_framework_zoneworld_zone_node" \
  --config="$CONFIG_DIR/zone-node-2.json"
wait_port "$NODE2_MESH"
sleep 1
curl --max-time 20 -fsS -X POST -H 'content-type: application/json' -d '{}' "${NODE2_HTTP/tcp:/http:}/bootstrap" >/dev/null
start_role ops "$BIN_DIR/sample_cpp_framework_zoneworld_ops" \
  --config="$CONFIG_DIR/ops.json"
wait_port "$OPS_STREAM"
start_role gateway "$BIN_DIR/sample_cpp_framework_zoneworld_gateway" \
  --config="$CONFIG_DIR/gateway.json"
wait_port "$GAME_STREAM"

sleep 5
"$BIN_DIR/sample_cpp_framework_zoneworld_client" --game-endpoint "$GAME_STREAM" \
  --ops-endpoint "$OPS_STREAM" |& tee "$LOG_DIR/client.log"
grep -q '^zoneworld=completed$' "$LOG_DIR/client.log"
grep -q '^zoneworld-relocation=completed$' "$LOG_DIR/client.log"
grep -q '^zoneworld-border=completed$' "$LOG_DIR/client.log"
grep -q '^zoneworld-ops=completed$' "$LOG_DIR/client.log"
for evidence in \
  "scenario ZW-A3 passed" \
  "scenario ZW-A5 passed" \
  "scenario ZW-B2 passed" \
  "scenario ZW-B6 passed" \
  "scenario ZW-B7 passed" \
  "scenario ZW-C1 passed" \
  "scenario ZW-D1 passed"; do
  grep -q "^${evidence}\$" "$LOG_DIR/client.log"
done
curl --max-time 30 -fsS -X POST -H 'content-type: application/json' -d '{}' "${GATEWAY_HTTP/tcp:/http:}/bootstrap-bots" >/dev/null
sleep 6
[[ "$(grep -h -c 'zoneworld-actor-joined.*player=bot-' "$LOG_DIR"/zone-node-*.log | awk '{ total += $1 } END { print total + 0 }')" -ge 8 ]]
grep -q 'zoneworld-bot-move player=bot-' "$LOG_DIR"/zone-node-*.log
grep -q 'zoneworld-border-received' "$LOG_DIR"/zone-node-*.log
grep -q 'zoneworld-zone-ready' "$LOG_DIR"/zone-node-*.log
echo "PASS ZoneWorld.Cpp"
echo "zoneworld sample result=passed"
