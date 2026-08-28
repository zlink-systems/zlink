#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../redis-common.sh"
CPP_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
source "$CPP_ROOT/samples/sample-build-common.sh"
B8_CHILD=0
G4_CHILD=0
case "${1:-}" in
  --b8-child)
    B8_CHILD=1
    shift
    ;;
  --g4-child)
    G4_CHILD=1
    shift
    ;;
esac
if [[ "$#" -ne 0 ]]; then
  echo "usage: $0 [--b8-child|--g4-child]" >&2
  exit 2
fi
zlink_cpp_sample_prepare_build "$CPP_ROOT"

cmake --build "$BUILD_DIR" --parallel 2 --target sample_cpp_framework_zoneworld_zone_node \
  sample_cpp_framework_zoneworld_gateway sample_cpp_framework_zoneworld_ops \
  sample_cpp_framework_zoneworld_client >/dev/null

B8_PROVEN=0
G4_PROVEN=0
if [[ "$B8_CHILD" == "0" && "$G4_CHILD" == "0" ]]; then
  bash "$0" --b8-child
  B8_PROVEN=1
  bash "$0" --g4-child
  G4_PROVEN=1
fi

RUN_DIR="$(mktemp -d)"
RUN_ID="$(basename "$RUN_DIR")-$$-${RANDOM}"
LOG_DIR="$RUN_DIR/logs"
mkdir -p "$LOG_DIR"
PIDS=()
declare -A ROLE_PID
REDIS_CONTAINER_NAME=""
cleanup() {
  local code=$?
  if [[ "$code" -ne 0 ]]; then
    for log in "$LOG_DIR"/*.log; do
      [[ -f "$log" ]] || continue
      echo "===== $log" >&2
      tail -n 400 "$log" >&2
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
  if [[ "$code" -ne 0 ]]; then
    echo "ZoneWorld run directory preserved: $RUN_DIR" >&2
  else
    rm -rf "$RUN_DIR"
  fi
  exit "$code"
}
trap cleanup EXIT INT TERM

read -r -a ZONEWORLD_PORTS <<<"$(zlink_sample_allocate_ports 16)"
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
NODE3_MESH="tcp://127.0.0.1:${ZONEWORLD_PORTS[13]}"
NODE3_STREAM="tcp://127.0.0.1:${ZONEWORLD_PORTS[14]}"
NODE3_HTTP="tcp://127.0.0.1:${ZONEWORLD_PORTS[15]}"
GATEWAY_MESH_BIND="$GATEWAY_MESH"
GATEWAY_MESH_ADVERTISE_HOST=""
NODE1_MESH_BIND="$NODE1_MESH"
NODE2_MESH_BIND="$NODE2_MESH"
NODE_MESH_ADVERTISE_HOST=""
if [[ "$B8_CHILD" == "1" ]]; then
  GATEWAY_MESH_BIND="tcp://127.0.0.2:${ZONEWORLD_PORTS[2]}"
  GATEWAY_MESH_ADVERTISE_HOST="127.0.0.1"
  NODE1_MESH_BIND="tcp://127.0.0.2:${ZONEWORLD_PORTS[0]}"
  NODE2_MESH_BIND="tcp://127.0.0.2:${ZONEWORLD_PORTS[1]}"
  NODE_MESH_ADVERTISE_HOST="127.0.0.1"
fi
zlink_redis_start_scoped_assign REDIS_CONTAINER_NAME redis_port "zlink-redis-cpp-sample-zoneworld" "redis:7-alpine"

CONFIG_DIR="$RUN_DIR/config"
mkdir -p "$CONFIG_DIR"
write_role_config() {
  local path="$1" node_id="$2" mesh_endpoint="$3" stream_endpoint="$4" http_endpoint="$5"
  local mesh_advertise_host="${6:-}" subscriber_only="${7:-false}" disable_bots="${8:-false}" allow_empty_zone_set="${9:-false}"
  python3 - "$path" "$node_id" "$mesh_endpoint" "$stream_endpoint" \
    "$http_endpoint" "tcp://127.0.0.1:${redis_port}" \
    "zoneworld:cpp:${RUN_ID}:" "$BROADCAST" "$LOG_DIR" "$mesh_advertise_host" \
    "$subscriber_only" "$disable_bots" "$allow_empty_zone_set" <<'CONFIG_PY'
import json
import os
import stat
import sys

path, node_id, mesh_endpoint, stream_endpoint, http_endpoint, redis_endpoint, redis_key_prefix, broadcast_endpoint, log_dir, mesh_advertise_host, subscriber_only, disable_bots, allow_empty_zone_set = sys.argv[1:]
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
        "faultTickZone": "zone-nw" if node_id.startswith("zone-node-") else None,
        "subscriberOnly": subscriber_only == "true",
        "disableBots": disable_bots == "true",
        "allowEmptyZoneSet": allow_empty_zone_set == "true",
        }
    }
}
if mesh_advertise_host:
    document["sample"]["zoneworld"]["meshAdvertiseHost"] = mesh_advertise_host
with open(path, "w", encoding="utf-8") as file:
    json.dump(document, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
CONFIG_PY
}

write_role_config "$CONFIG_DIR/zone-node-1.json" zone-node-1 "$NODE1_MESH_BIND" \
  "$NODE1_STREAM" "${NODE1_HTTP/tcp:/http:}" "$NODE_MESH_ADVERTISE_HOST"
write_role_config "$CONFIG_DIR/zone-node-2.json" zone-node-2 "$NODE2_MESH_BIND" \
  "$NODE2_STREAM" "${NODE2_HTTP/tcp:/http:}" "$NODE_MESH_ADVERTISE_HOST"
write_role_config "$CONFIG_DIR/zone-node-1-replacement.json" zone-node-1 "$NODE1_MESH_BIND" \
  "$NODE1_STREAM" "${NODE1_HTTP/tcp:/http:}" "$NODE_MESH_ADVERTISE_HOST" false true true
write_role_config "$CONFIG_DIR/zone-node-2-replacement.json" zone-node-2 "$NODE2_MESH_BIND" \
  "$NODE2_STREAM" "${NODE2_HTTP/tcp:/http:}" "$NODE_MESH_ADVERTISE_HOST" false true true
write_role_config "$CONFIG_DIR/ops.json" ops "$OPS_MESH" "$OPS_STREAM" \
  "${OPS_HTTP/tcp:/http:}"
write_role_config "$CONFIG_DIR/gateway.json" gateway "$GATEWAY_MESH_BIND" "$GAME_STREAM" \
  "${GATEWAY_HTTP/tcp:/http:}" "$GATEWAY_MESH_ADVERTISE_HOST"
write_role_config "$CONFIG_DIR/zone-node-3.json" zone-node-3 "$NODE3_MESH" \
  "$NODE3_STREAM" "${NODE3_HTTP/tcp:/http:}" "" true true

start_role() {
  local label="$1"
  shift
  env "$@" >>"$LOG_DIR/$label.log" 2>&1 &
  local pid=$!
  PIDS+=("$pid")
  ROLE_PID["$label"]="$pid"
}
remove_owned_pid() {
  local completed_pid="$1" active=() pid
  for pid in "${PIDS[@]:-}"; do
    [[ "$pid" == "$completed_pid" ]] || active+=("$pid")
  done
  PIDS=("${active[@]}")
}
stop_role() {
  local label="$1" signal="${2:-KILL}"
  local pid="${ROLE_PID[$label]:-}"
  [[ -n "$pid" ]] || return 0
  kill "-$signal" "$pid" >/dev/null 2>&1 || true
  wait "$pid" >/dev/null 2>&1 || true
  remove_owned_pid "$pid"
  unset "ROLE_PID[$label]"
}
next_log_line() {
  local path="$1"
  if [[ -f "$path" ]]; then
    echo $(( $(wc -l <"$path") + 1 ))
  else
    echo 1
  fi
}
wait_for_log_after() {
  local label="$1" pattern="$2" first_line="$3" attempts="${4:-600}"
  for _ in $(seq 1 "$attempts"); do
    if [[ -f "$LOG_DIR/$label.log" ]] \
       && tail -n +"$first_line" "$LOG_DIR/$label.log" | grep -Fq "$pattern"; then
      return 0
    fi
    sleep 0.1
  done
  return 1
}
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
start_zone_node() {
  local label="$1" config_label="${2:-$1}" first_line
  first_line="$(next_log_line "$LOG_DIR/$label.log")"
  start_role "$label" "$BIN_DIR/sample_cpp_framework_zoneworld_zone_node" \
    --config="$CONFIG_DIR/$config_label.json"
  if [[ "$label" == "zone-node-1" ]]; then
    wait_port "$NODE1_MESH"
  elif [[ "$label" == "zone-node-2" ]]; then
    wait_port "$NODE2_MESH"
  fi
  wait_for_log_after "$label" "topology=ready node=$label zones=" "$first_line" 300
}
routing_id_of() {
  local node_id="$1"
  sed -nE "s/.*zoneworld-node-report node=${node_id} rid=([^ ]+).*/\1/p" \
    "$LOG_DIR/ops.log" | tail -1
}
wait_for_new_routing_id() {
  local node_id="$1" previous="$2" candidate="" observations=0
  for _ in $(seq 1 300); do
    candidate="$(routing_id_of "$node_id")"
    if [[ -n "$candidate" && "$candidate" != "$previous" ]]; then
      observations="$(grep -Fc \
        "zoneworld-node-report node=$node_id rid=$candidate registered=true" \
        "$LOG_DIR/ops.log" || true)"
      if [[ "$observations" -ge 2 ]]; then
        echo "$candidate"
        return 0
      fi
    fi
    sleep 0.1
  done
  return 1
}
run_client_lane() {
  local label="$1"
  shift
  "$BIN_DIR/sample_cpp_framework_zoneworld_client" --game-endpoint "$GAME_STREAM" \
    --ops-endpoint "$OPS_STREAM" "$@" >>"$LOG_DIR/client-$label.log" 2>&1
}

if [[ "$B8_CHILD" == "1" ]]; then
  for proxy_index in 0 1 2; do
    start_role "session-route-proxy-$proxy_index" \
      python3 "$SCRIPT_DIR/Support/session_route_block_proxy.py" \
      --listen-host 127.0.0.1 --listen-port "${ZONEWORLD_PORTS[$proxy_index]}" \
      --target-host 127.0.0.2 --target-port "${ZONEWORLD_PORTS[$proxy_index]}" \
      --arm-file "$RUN_DIR/b8-block-command-44"
    wait_for_log_after "session-route-proxy-$proxy_index" "proxy-ready" 1 300
  done
  start_zone_node zone-node-1
  start_zone_node zone-node-2
else
  # G2: vary the canonical startup order while keeping NodeId stable and RID generated.
  start_zone_node zone-node-2
  start_zone_node zone-node-1
fi
start_role ops "$BIN_DIR/sample_cpp_framework_zoneworld_ops" \
  --config="$CONFIG_DIR/ops.json"
wait_port "$OPS_STREAM"
start_role gateway "$BIN_DIR/sample_cpp_framework_zoneworld_gateway" \
  --config="$CONFIG_DIR/gateway.json"
wait_port "$GAME_STREAM"

sleep 2
curl --max-time 30 -fsS -X POST -H 'content-type: application/json' -d '{}' "${GATEWAY_HTTP/tcp:/http:}/bootstrap-world" >/dev/null
sleep 6

if [[ "$B8_CHILD" == "1" ]]; then
  start_role client-b8 "$BIN_DIR/sample_cpp_framework_zoneworld_client" \
    --game-endpoint "$GAME_STREAM" --ops-endpoint "$OPS_STREAM" --scenario B8 \
    --arm-file "$RUN_DIR/b8-block-command-44"
  if ! wait_for_log_after client-b8 "scenario ZW-B8 armed" 1 600; then
    echo "zoneworld-b8=failed reason=client-not-armed" >&2
    exit 1
  fi
  touch "$RUN_DIR/b8-block-command-44"
  b8_pid="${ROLE_PID[client-b8]}"
  set +e
  wait "$b8_pid"
  b8_status=$?
  set -e
  remove_owned_pid "$b8_pid"
  unset "ROLE_PID[client-b8]"
  if [[ "$b8_status" -ne 0 ]] \
     || ! grep -Fq "scenario ZW-B8 passed" "$LOG_DIR/client-b8.log" \
     || ! grep -hFq "blocked-command-44" "$LOG_DIR"/session-route-proxy-*.log; then
    echo "zoneworld-b8=failed reason=seal-timeout-or-reconnect" >&2
    exit 1
  fi
  echo "scenario ZW-B8 passed"
  echo "zoneworld-b8=completed"
  exit 0
fi

if [[ "$G4_CHILD" == "1" ]]; then
  g4_node1_line="$(next_log_line "$LOG_DIR/zone-node-1.log")"
  g4_node2_line="$(next_log_line "$LOG_DIR/zone-node-2.log")"
  start_role client-g4 "$BIN_DIR/sample_cpp_framework_zoneworld_client" \
    --game-endpoint "$GAME_STREAM" --ops-endpoint "$OPS_STREAM" --scenario G4
  g4_pid="${ROLE_PID[client-g4]}"
  if ! wait_for_log_after client-g4 "scenario ZW-G4 armed node=" 1 600; then
    echo "zoneworld-g4=failed reason=client-not-armed" >&2
    exit 1
  fi
  g4_node="$(sed -nE 's/.*scenario ZW-G4 armed node=([^ ]+).*/\1/p' \
    "$LOG_DIR/client-g4.log" | tail -1)"
  if [[ "$g4_node" == "zone-node-1" ]]; then
    g4_target_line="$g4_node1_line"
  else
    g4_target_line="$g4_node2_line"
  fi
  g4_old_rid="$(routing_id_of "$g4_node")"
  if [[ "$g4_node" != "zone-node-1" && "$g4_node" != "zone-node-2" ]] \
     || ! wait_for_log_after "$g4_node" "zoneworld-crash-boundary join pending" \
       "$g4_target_line" 600; then
    echo "zoneworld-g4=failed reason=crash-boundary-not-reached" >&2
    exit 1
  fi
  stop_role "$g4_node" KILL
  set +e
  wait "$g4_pid"
  g4_client_status=$?
  set -e
  remove_owned_pid "$g4_pid"
  unset "ROLE_PID[client-g4]"
  start_zone_node "$g4_node" "$g4_node-replacement"
  if ! g4_new_rid="$(wait_for_new_routing_id "$g4_node" "$g4_old_rid")"; then
    echo "zoneworld-g4=failed reason=replacement-not-observed" >&2
    exit 1
  fi
  set +e
  run_client_lane g4-fresh --scenario G4-fresh
  g4_fresh_status=$?
  set -e
  if [[ "$g4_client_status" -ne 0 || "$g4_fresh_status" -ne 0 \
        || ! "$g4_new_rid" =~ ^zn-[0-9a-f-]{36}$ ]] \
     || ! grep -Fq "scenario ZW-G4-boundary passed" "$LOG_DIR/client-g4.log" \
     || ! grep -Fq "fresh-actor-proof scenario=G4-fresh" \
       "$LOG_DIR/client-g4-fresh.log" \
     || ! grep -Fq "owner=$g4_new_rid" "$LOG_DIR/client-g4-fresh.log"; then
    echo "zoneworld-g4=failed reason=boundary-or-fresh-actor-proof" >&2
    exit 1
  fi
  echo "scenario ZW-G4 passed"
  echo "zoneworld-g4=completed"
  exit 0
fi

# Canonical §11.2 verdict ledger. A phase marker is emitted only when every
# constituent scenario passed; zoneworld=completed is the AND of this table.
EXPECTED_IDS=(
  ZW-A1 ZW-A2 ZW-A3 ZW-A4 ZW-A5
  ZW-B1 ZW-B2 ZW-B3 ZW-B4 ZW-B5 ZW-B6 ZW-B7 ZW-B8
  ZW-C1 ZW-C2 ZW-C3 ZW-C4
  ZW-D1 ZW-D2
  ZW-E1 ZW-E2 ZW-E3 ZW-E4 ZW-E5 ZW-E6
  ZW-F1 ZW-F2 ZW-F3 ZW-F4
  ZW-G1 ZW-G2 ZW-G3 ZW-G4 ZW-G5
)
declare -A VERDICT DETAIL
record_verdict() {
  local id="$1" verdict="$2" detail="${3:-}"
  VERDICT["$id"]="$verdict"
  DETAIL["$id"]="$detail"
}

set +e
"$BIN_DIR/sample_cpp_framework_zoneworld_client" --game-endpoint "$GAME_STREAM" \
  --ops-endpoint "$OPS_STREAM" |& tee "$LOG_DIR/client.log"
CLIENT_STATUS=${PIPESTATUS[0]}
set -e

# Capture F2 before runner-driven process stops can abort unrelated bot joins during teardown.
MAIN_BOT_JOIN_FAILURES="$(awk \
  '/zoneworld-join-failed player=bot-/{count++} END{print count + 0}' \
  "$LOG_DIR"/zone-node-[12].log)"
MAIN_BOT_RELOCATION=0
if grep -hEq 'zoneworld-actor-joined.*player=bot-.*initial=false' \
  "$LOG_DIR"/zone-node-[12].log; then
  MAIN_BOT_RELOCATION=1
fi

D2_STATUS=1
start_zone_node zone-node-3
sleep 1
set +e
run_client_lane d2 --scenario D2
D2_CLIENT_STATUS=$?
set -e
if [[ "$D2_CLIENT_STATUS" -eq 0 ]]; then
  d2_announcement="$(sed -nE 's/^announcement-proof id=(.+)$/\1/p' \
    "$LOG_DIR/client-d2.log" | tail -1)"
  if [[ -n "$d2_announcement" ]] \
     && grep -Fq "zoneworld-fanout-announcement node=zone-node-3 id=$d2_announcement" \
       "$LOG_DIR/zone-node-3.log"; then
    D2_STATUS=0
  fi
fi

G4_STATUS=1
[[ "$G4_PROVEN" == 1 ]] && G4_STATUS=0

TRANSITION_STATUS=1
start_role client-transition "$BIN_DIR/sample_cpp_framework_zoneworld_client" \
  --game-endpoint "$GAME_STREAM" --ops-endpoint "$OPS_STREAM" --scenario transition
transition_pid="${ROLE_PID[client-transition]}"
if wait_for_log_after client-transition "scenario ZW-B4-C3 armed node=" 1 600; then
  transition_node="$(sed -nE \
    's/.*scenario ZW-B4-C3 armed node=([^ ]+).*/\1/p' \
    "$LOG_DIR/client-transition.log" | tail -1)"
  if [[ "$transition_node" == "zone-node-1" || "$transition_node" == "zone-node-2" ]]; then
    stop_role "$transition_node" KILL
    set +e
    wait "$transition_pid"
    transition_client_status=$?
    set -e
    remove_owned_pid "$transition_pid"
    unset "ROLE_PID[client-transition]"
    if [[ "$transition_client_status" -eq 0 ]] \
       && grep -Fq "scenario ZW-B4 passed" "$LOG_DIR/client-transition.log" \
       && grep -Fq "scenario ZW-C3 passed" "$LOG_DIR/client-transition.log"; then
      TRANSITION_STATUS=0
    fi
    # A SIGKILLed owner does not hand its zones back: spec 2.2 says a Ready owner failure is
    # never an automatic replacement. The node comes back as a crash replacement with no zones,
    # which is what ZW-G4 already does and what java/kotlin do for every restart.
    start_zone_node "$transition_node" "$transition_node-replacement"
    sleep 2
  fi
fi

E5_STATUS=1
E5_NODE="${transition_node:-zone-node-2}"
set +e
run_client_lane e5-arm --scenario E5-arm --target-node-id "$E5_NODE"
e5_arm_status=$?
set -e
if [[ "$e5_arm_status" -eq 0 ]]; then
  stop_role "$E5_NODE" KILL
  start_zone_node "$E5_NODE" "$E5_NODE-replacement"
  sleep 2
  set +e
  run_client_lane e5 --scenario E5 --target-node-id "$E5_NODE"
  e5_restore_status=$?
  set -e
  if [[ "$e5_restore_status" -eq 0 ]] \
     && grep -Fq "scenario ZW-E5 passed" "$LOG_DIR/client-e5.log"; then
    E5_STATUS=0
  fi
fi

G3_STATUS=1
G3_NODE=zone-node-1
g3_old_rid="$(routing_id_of "$G3_NODE")"
if [[ -n "$g3_old_rid" ]]; then
  stop_role "$G3_NODE" TERM
  start_zone_node "$G3_NODE" "$G3_NODE-replacement"
  if g3_new_rid="$(wait_for_new_routing_id "$G3_NODE" "$g3_old_rid")"; then
    set +e
    run_client_lane g3 --scenario G3
    g3_client_status=$?
    set -e
    if [[ "$g3_client_status" -eq 0 && "$g3_new_rid" =~ ^zn-[0-9a-f-]{36}$ ]] \
       && grep -Fq "fresh-actor-proof scenario=G3" "$LOG_DIR/client-g3.log" \
       && grep -Fq "owner=$g3_new_rid" "$LOG_DIR/client-g3.log"; then
      G3_STATUS=0
    fi
  fi
fi

G2_STATUS=1
g2_node1_rid="$(routing_id_of zone-node-1)"
g2_node2_rid="$(routing_id_of zone-node-2)"
if [[ "$g2_node1_rid" =~ ^zn-[0-9a-f-]{36}$ \
      && "$g2_node2_rid" =~ ^zn-[0-9a-f-]{36}$ \
      && "$g2_node1_rid" != "$g2_node2_rid" ]]; then
  G2_STATUS=0
fi

# ZW-C2 runs last and on its own. Its precondition is a *normal* shutdown, which the abrupt
# B4/C3 stop cannot produce, and the sample spec's stop table fixes the two as different signals.
# Placed after G2 so no earlier scenario depends on this node still being up.
C2_NODE=zone-node-2
start_role client-c2 "$BIN_DIR/sample_cpp_framework_zoneworld_client" \
  --game-endpoint "$GAME_STREAM" --ops-endpoint "$OPS_STREAM" \
  --scenario C2 --target-node-id "$C2_NODE"
c2_pid="${ROLE_PID[client-c2]}"
if wait_for_log_after client-c2 "scenario ZW-C2 armed node=" 1 600; then
  stop_role "$C2_NODE" TERM
  set +e
  wait "$c2_pid"
  set -e
  remove_owned_pid "$c2_pid"
  unset "ROLE_PID[client-c2]"
  start_zone_node "$C2_NODE" "$C2_NODE-replacement"
fi

for id in "${EXPECTED_IDS[@]}"; do
  if grep -hFxq "scenario $id passed" "$LOG_DIR"/client*.log; then
    record_verdict "$id" PASS "typed client assertion"
  fi
done
[[ "$B8_PROVEN" == 1 ]] && record_verdict ZW-B8 PASS "command-44 seal timeout and reconnect"
[[ "$D2_STATUS" == 0 ]] && record_verdict ZW-D2 PASS "third subscriber received reannounce"
if [[ "$TRANSITION_STATUS" == 0 ]]; then
  record_verdict ZW-B4 PASS "border snapshot expired after publisher stop"
  record_verdict ZW-C3 PASS "report TTL expiry observed"
fi
[[ "$E5_STATUS" == 0 ]] && record_verdict ZW-E5 PASS "maintenance restored after restart"
[[ "$G2_STATUS" == 0 ]] && record_verdict ZW-G2 PASS "reverse startup order remained routable"
[[ "$G3_STATUS" == 0 ]] && record_verdict ZW-G3 PASS "normal replacement advanced RID and accepted fresh object"
[[ "$G4_STATUS" == 0 ]] && record_verdict ZW-G4 PASS "crash boundary stayed Unavailable and replacement accepted fresh object"

spawned="$(grep -h 'zoneworld-bot-spawned bot=' "$LOG_DIR/gateway.log" \
  | sed -E 's/.*bot=([^ ]+).*/\1/' | sort -u | wc -l)"
if [[ "$spawned" -eq 8 ]] \
   && grep -Fq 'bot=bot-nw-x zone=zone-nw start=10,15 dir=1,0' "$LOG_DIR/gateway.log" \
   && grep -Fq 'bot=bot-se-y zone=zone-se start=85,90 dir=0,-1' "$LOG_DIR/gateway.log"; then
  record_verdict ZW-F1 PASS "eight fixed bots and endpoint trajectories observed"
else
  record_verdict ZW-F1 FAIL "expected eight canonical bot spawns, observed $spawned"
fi

# Bots retry a rejected join, so a single observed completion says nothing on
# its own — hundreds of failures can hide behind it. F2 therefore counts the
# bot join failures in the window as well: relocation evidence is only real
# when it is failure free.
bot_join_failures="$MAIN_BOT_JOIN_FAILURES"
if [[ "$MAIN_BOT_RELOCATION" -ne 1 ]]; then
  record_verdict ZW-F2 FAIL "no unbound bot relocation completion"
elif [[ "$bot_join_failures" -ne 0 ]]; then
  record_verdict ZW-F2 FAIL \
    "bot relocation completed only behind $bot_join_failures join failures"
else
  record_verdict ZW-F2 PASS "unbound bot relocation completed with no join failure"
fi
if grep -hEq 'zoneworld-bot-reversed player=bot-.*reason=(OutOfRange|TooFar)' "$LOG_DIR"/zone-node-*.log; then
  record_verdict ZW-F3 PASS "bot direction reversal observed at rejection"
else
  record_verdict ZW-F3 FAIL "bot rejection did not expose a direction reversal"
fi
if ! grep -hEq 'bound_session_push actor=bot-|push-to-bot' "$LOG_DIR"/*.log; then
  record_verdict ZW-F4 PASS "no bound-session push was attempted for a bot"
else
  record_verdict ZW-F4 FAIL "a bot received a bound-session push attempt"
fi

if grep -Fq 'set_automatic_routing_id_prefix ("zn")' "$SCRIPT_DIR/Server/ZoneNode/main.cpp" \
   && ! grep -Eq 'set_(automatic_)?routing_id *\(' "$SCRIPT_DIR/Server/ZoneNode/main.cpp"; then
  record_verdict ZW-G5 PASS "ZoneNode uses generated routing ids only"
else
  # The allowed prefix setter is not a fixed physical RID.
  if grep -Fq 'set_automatic_routing_id_prefix ("zn")' "$SCRIPT_DIR/Server/ZoneNode/main.cpp" \
     && ! grep -Eq 'set_routing_id *\(' "$SCRIPT_DIR/Server/ZoneNode/main.cpp"; then
    record_verdict ZW-G5 PASS "ZoneNode uses generated routing ids only"
  else
    record_verdict ZW-G5 FAIL "fixed or non-canonical ZoneNode routing configuration"
  fi
fi

# Preserve the first client-visible failure and do not overwrite a typed PASS
# marker emitted before a later scenario failed.
if [[ -z "${VERDICT[ZW-A1]:-}" && "$CLIENT_STATUS" -ne 0 ]] \
   && grep -hFq 'zoneworld-join-accepted player=player-alice' "$LOG_DIR"/zone-node-*.log \
   && grep -Fq 'stream connector wait timed out' "$LOG_DIR/client.log"; then
  record_verdict ZW-A1 BLOCKED "Framework deferred admission completed but JoinWorldRes/bound push did not commit; run-dir=$RUN_DIR"
fi
if [[ "${VERDICT[ZW-A1]:-}" == PASS \
      && "${VERDICT[ZW-A4]:-}" == PASS \
      && "${VERDICT[ZW-A5]:-}" == PASS \
      && -z "${VERDICT[ZW-A2]:-}" \
      && "$CLIENT_STATUS" -ne 0 \
      && $(tail -1 "$LOG_DIR/client.log") == *'stream connector wait timed out'* ]]; then
  record_verdict ZW-A2 FAIL "same-zone MoveMsg did not produce the awaited ZoneStateNotify; run-dir=$RUN_DIR"
fi
for id in ZW-A2 ZW-A3 ZW-A4 ZW-A5 ZW-B1 ZW-B2 ZW-B3 ZW-B4 ZW-B5 ZW-B6 ZW-B7 ZW-B8 ZW-D1 ZW-E2 ZW-E3 ZW-E4; do
  if [[ -z "${VERDICT[$id]:-}" ]]; then
    if [[ "${VERDICT[ZW-A1]:-}" != PASS ]]; then
      record_verdict "$id" BLOCKED "depends on ZW-A1 bound-session readiness"
    else
      record_verdict "$id" BLOCKED "not reached after the first later scenario failure"
    fi
  fi
done
if [[ -z "${VERDICT[ZW-C4]:-}" ]]; then
  if grep -hFq 'zlink.runtime.spot.timer_failed' "$LOG_DIR"/zone-node-*.log \
     && grep -Fq 'zoneworld-node-alert ' "$LOG_DIR/ops.log"; then
    record_verdict ZW-C4 PASS "public Spot timer event reached the Ops handler"
  else
    record_verdict ZW-C4 FAIL "public Spot timer event did not reach the Ops handler"
  fi
fi
for id in ZW-C2 ZW-C3 ZW-D2 ZW-E5 ZW-G2 ZW-G3 ZW-G4; do
  [[ -n "${VERDICT[$id]:-}" ]] || record_verdict "$id" FAIL "runner-driven lifecycle lane was not reached"
done

all_passed=true
for id in "${EXPECTED_IDS[@]}"; do
  verdict="${VERDICT[$id]:-FAIL}"
  detail="${DETAIL[$id]:-no verdict was recorded}"
  printf 'scenario-ledger id=%s verdict=%s detail=%s\n' "$id" "$verdict" "$detail"
  [[ "$verdict" == PASS ]] || all_passed=false
done
if [[ "$all_passed" == true ]]; then
  echo "zoneworld=completed"
  echo "PASS ZoneWorld.Cpp"
  echo "zoneworld sample result=passed"
  exit 0
fi
echo "zoneworld=blocked run-dir=$RUN_DIR"
exit 1
