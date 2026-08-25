#!/usr/bin/env bash
# ZoneWorld (dotnet). Brings up the topology in the order §12 fixes, then runs the
umask 077
# scenario client against it.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../redis-common.sh"
# 샘플 검증은 client connector 경로만 돌린다. node·cpp ZoneWorld runner에는 브라우저
# 단계가 없으므로 기본 실행 범위를 맞춘다. 브라우저 smoke는 --browser-smoke로 따로 켠다.
BROWSER_SMOKE=0
SCENARIO="all"
SCENARIO_SET=0
G4_CHILD=0
G4_PROVEN=0
B8_CHILD=0
B8_PROVEN=0
TRACE_STREAM="${ZLINK_SAMPLE_TRACE_STREAM:-0}"
SPOT_DISCOVERY_TRACE="${ZLINK_SAMPLE_SPOT_DISCOVERY_TRACE:-1}"
while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --browser-smoke)
      BROWSER_SMOKE=1
      shift
      ;;
    --no-browser-smoke)
      BROWSER_SMOKE=0
      shift
      ;;
    --g4-child)
      G4_CHILD=1
      shift
      ;;
    --b8-child)
      B8_CHILD=1
      shift
      ;;
    --*)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
    *)
      if [[ "$SCENARIO_SET" == "1" ]]; then
        echo "Only one scenario selector may be supplied." >&2
        exit 2
      fi
      SCENARIO="$1"
      SCENARIO_SET=1
      shift
      ;;
  esac
done

scenario_selected() {
  [[ "$SCENARIO" == "all" || ",${SCENARIO}," == *",$1,"* ]]
}

# Run the crash replacement in a separate process and Redis scope so its stale descriptor
# history cannot affect the normal replacement scenario.
if [[ "$G4_CHILD" == "0" ]] && scenario_selected ZW-G4; then
  bash "$0" --g4-child ZW-G4
  G4_PROVEN=1
  if [[ "$SCENARIO" == "ZW-G4" ]]; then exit 0; fi
fi
# ZW-B8 intentionally drops the internal session-route command. Keep that destructive
# transport fault in its own topology so the canonical lane that follows starts clean.
if [[ "$B8_CHILD" == "0" ]] && scenario_selected ZW-B8; then
  bash "$0" --b8-child ZW-B8
  B8_PROVEN=1
  if [[ "$SCENARIO" == "ZW-B8" ]]; then exit 0; fi
fi
RUN_DIR="$(mktemp -d)"
RUN_DIR="$(cd "${RUN_DIR}" && pwd)"
RUN_ID="$(basename "$RUN_DIR")-$$-$RANDOM"
LOG_DIR="$RUN_DIR/logs"
CONFIG_DIR="$RUN_DIR/config"
mkdir -p "$LOG_DIR" "$CONFIG_DIR"

PIDS=()
REDIS_CONTAINER=""

cleanup() {
  find "${RUN_DIR}" -type f -name "*.json" -delete 2>/dev/null || true
  for pid in "${PIDS[@]:-}"; do
    # Scenario shutdown semantics are tested explicitly below. EXIT cleanup only owns test
    # processes, so stop them immediately instead of entering every server's bounded drain and
    # making a completed or failed runner wait for several overlapping drain windows.
    kill -KILL "$pid" 2>/dev/null || true
  done
  for pid in "${PIDS[@]:-}"; do
    wait "$pid" 2>/dev/null || true
  done
  if [[ -n "$REDIS_CONTAINER" ]]; then
    zlink_redis_remove_by_id "$REDIS_CONTAINER" || true
  fi
  zlink_sample_copy_evidence "$RUN_DIR" "ZoneWorld"
}
trap cleanup EXIT

if ! command -v docker >/dev/null 2>&1; then
  echo "Docker is required to run ZoneWorld (it provisions a dedicated Redis container)." >&2
  exit 1
fi
REDIS_CONTAINER="zlink-zoneworld-dotnet-redis-$RUN_ID"
zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_ENDPOINT \
  "zlink-zoneworld-dotnet-redis" redis:7.2-alpine

echo "==> build"
dotnet build "$ROOT_DIR/Server/Ops/ZoneWorld.Server.Ops.csproj" --maxcpucount:1 -v q --nologo >/dev/null
dotnet build "$ROOT_DIR/Server/ZoneNode/ZoneWorld.Server.ZoneNode.csproj" --maxcpucount:1 -v q --nologo >/dev/null
dotnet build "$ROOT_DIR/Server/Gateway/ZoneWorld.Server.Gateway.csproj" --maxcpucount:1 -v q --nologo >/dev/null
dotnet build "$ROOT_DIR/Client/ZoneWorld.Client.csproj" --maxcpucount:1 -v q --nologo >/dev/null

read -r -a PORTS <<<"$(python3 - <<'PY'
import random
import socket

sockets = []
chosen = set()
try:
    while len(sockets) < 9:
        port = random.randint(22100, 23999)
        if port in chosen:
            continue
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind(("127.0.0.1", port))
        except OSError:
            sock.close()
            continue
        chosen.add(port)
        sockets.append(sock)
    print(" ".join(str(sock.getsockname()[1]) for sock in sockets))
finally:
    for sock in sockets:
        sock.close()
PY
)"

GATEWAY_ENDPOINT="ws://127.0.0.1:${PORTS[6]}"
OPS_ENDPOINT="ws://127.0.0.1:${PORTS[4]}"
BROWSER_PREVIEW_PORT="${PORTS[8]}"

python3 - "$CONFIG_DIR" "$REDIS_ENDPOINT" "$RUN_ID" "$LOG_DIR" "$B8_CHILD" "${PORTS[@]}" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
redis = sys.argv[2]
run_id = sys.argv[3]
log_dir = sys.argv[4]
b8_child = sys.argv[5] == "1"
ports = [int(port) for port in sys.argv[6:]]
shared = {
    "redisEndpoint": redis,
    "redisKeyPrefix": f"zoneworld-{run_id}:",
    "logDirectory": log_dir,
}

def write(name, role, value):
    (root / f"{name}.json").write_text(json.dumps({"shared": shared, role: value}), encoding="utf-8")

for index in (1, 2, 3):
    write(f"zone-node-{index}", "zoneNode", {
        "nodeId": f"zone-node-{index}",
        "meshEndpoint": f"tcp://{'127.0.0.2' if b8_child and index < 3 else '127.0.0.1'}:{ports[index - 1]}",
        "meshAdvertiseHost": "127.0.0.1" if b8_child and index < 3 else None,
        "faultTickZone": "zone-nw" if index in (1, 2) else None,
        "disableBots": False,
        "subscriberOnly": index == 3,
    })

# The replacement reuses the application NodeId but has a different socket endpoint.
# Framework gives every process lifecycle a new prefix-based RID.
write("zone-node-crash-replacement", "zoneNode", {
    "nodeId": "zone-node-2",
    "meshEndpoint": f"tcp://127.0.0.1:{ports[3]}",
    "faultTickZone": None,
    "disableBots": True,
    "subscriberOnly": False,
    # 7.5 — crash 교체는 이전 owner의 object를 복원하지 않는다. zone 0개로 ready가 된다.
    "allowEmptyZoneSet": True,
})

write("zone-node-replacement", "zoneNode", {
    "nodeId": "zone-node-2",
    "meshEndpoint": f"tcp://127.0.0.1:{ports[3]}",
    "faultTickZone": None,
    "disableBots": False,
    "subscriberOnly": False,
})

write("ops", "ops", {
    "streamEndpoint": f"ws://127.0.0.1:{ports[4]}",
    "meshEndpoint": f"tcp://127.0.0.1:{ports[5]}",
})
write("gateway", "gateway", {
    "streamEndpoint": f"ws://127.0.0.1:{ports[6]}",
    "meshEndpoint": f"tcp://{'127.0.0.2' if b8_child else '127.0.0.1'}:{ports[7]}",
    "meshAdvertiseHost": "127.0.0.1" if b8_child else None,
})
PY

declare -A NODE_PID

# `dotnet run` is a wrapper: killing it does not always take the server it launched with it,
# and a survivor keeps the node's routing id bound (see the orphan check above). Launch the
# built binary so the pid we record is the server itself.
BIN_DIR="bin/Debug/net8.0"
SERVER_BIN="$ROOT_DIR/Server/ZoneNode/$BIN_DIR/ZoneWorld.Server.ZoneNode"
OPS_BIN="$ROOT_DIR/Server/Ops/$BIN_DIR/ZoneWorld.Server.Ops"
GATEWAY_BIN="$ROOT_DIR/Server/Gateway/$BIN_DIR/ZoneWorld.Server.Gateway"
CLIENT_BIN="$ROOT_DIR/Client/$BIN_DIR/ZoneWorld.Client"

start() {
  local name="$1"; shift
  # Keep Framework spot-discovery evidence in the same per-process files that the
  # readiness gates inspect. This makes mesh admission and replacement routing
  # observable without changing the sample's public traffic.
  if [[ "$SPOT_DISCOVERY_TRACE" == "1" ]]; then
    ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1 "$@" >>"$LOG_DIR/$name.log" 2>&1 &
  else
    "$@" >>"$LOG_DIR/$name.log" 2>&1 &
  fi
  PIDS+=($!)
  NODE_PID[$name]=$!
  echo "    started $name (pid ${PIDS[-1]})"
}

remove_owned_pid() {
  local completed_pid="$1"
  local active=()
  local pid
  for pid in "${PIDS[@]:-}"; do
    [[ "$pid" == "$completed_pid" ]] || active+=("$pid")
  done
  PIDS=("${active[@]}")
}

# ZW-B4·ZW-C2·ZW-E5 assert what happens when a node goes away, so the runner has to take one
# away. The client cannot: it only speaks to Gateway and Ops.
stop_node() {
  local name="$1"
  local pid="${NODE_PID[$name]:-}"
  [[ -n "$pid" ]] || return 0
  echo "    stopping $name (pid $pid)"
  # These scenarios model a node disappearing, not a graceful deployment drain. SIGTERM keeps
  # the framework's owner lease alive while its bounded drain runs (about 30 seconds here), so
  # the node is still registered for most of the client's observation window. An abrupt stop
  # leaves the last lease behind and lets its configured TTL drive the automatic unregister.
  kill -KILL "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  remove_owned_pid "$pid"
  unset "NODE_PID[$name]"
}

graceful_stop_node() {
  local name="$1"
  local pid="${NODE_PID[$name]:-}"
  [[ -n "$pid" ]] || return 0
  echo "    draining $name (pid $pid)"
  kill -TERM "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  remove_owned_pid "$pid"
  unset "NODE_PID[$name]"
}

routing_id_of() {
  local node_id="$1" first_line="${2:-1}"
  tail -n +"$first_line" "$LOG_DIR/ops.log" \
    | sed -nE "s/.*node status observed\\. node=${node_id}, rid=([^ ,]+).*/\\1/p" \
    | tail -1
}

is_zone_node_rid() {
  [[ "$1" =~ ^zn-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$ ]]
}

start_zone_node() {
  local name="$1"
  local config_name="${2:-$name}"
  local first_new_line first_new_ops_line first_peer_line
  local local_rid peer_rid
  # A restarted zone-node-2 uses the replacement endpoint. Reusing the original
  # config would publish a new RID on the retired socket and would not exercise
  # the different-endpoint replacement contract.
  if [[ "$name" == "zone-node-2" && "$config_name" == "$name" ]]; then
    config_name="zone-node-replacement"
  fi
  first_new_line="$(next_log_line "$LOG_DIR/$name.log")"
  first_new_ops_line="$(next_log_line "$LOG_DIR/ops.log")"
  first_peer_line="$(next_log_line "$LOG_DIR/zone-node-1.log")"
  : >"$LOG_DIR/$name.restart.marker"
  start "$name" "$SERVER_BIN" --config "$CONFIG_DIR/$config_name.json"
  # A restarted process gets a new RID. Wait for application readiness and the two independent
  # observations that prove Ops has received its report and accepted its new connection.
  wait_for_log_after "$name" "topology=ready" "$first_new_line" 450
  # These two independent observations replace a fixed convergence delay: the restarted node
  # has submitted a status report, and Ops has observed the new socket connection.
  wait_for_log_after "$name" "node status report submitted. node=$name" "$first_new_line" 450
  wait_for_log_after ops "node connection observed. node=$name, connected=True" "$first_new_ops_line" 450

  # Process readiness is complete only after the replacement RID has completed the mesh
  # admission handshake with node-1. The status report and Ops socket observation above prove
  # discovery and transport reachability, but they do not prove that routed application traffic
  # can use the new peer.
  if [[ "$name" == "zone-node-2" ]]; then
    local_rid="$(routing_id_of zone-node-2 "$first_new_ops_line")"
    peer_rid="$(routing_id_of zone-node-1)"
    wait_for_peer_admission_after \
      "$name" "$local_rid" "$first_new_line" \
      zone-node-1 "$peer_rid" "$first_peer_line" 600
  fi
}

client_config() {
  local scenarios="$1" path="$CONFIG_DIR/client.json"
  python3 - "$CONFIG_DIR/ops.json" "$path" "$scenarios" "$GATEWAY_ENDPOINT" "$OPS_ENDPOINT" \
    "$TRACE_STREAM" "$RUN_DIR/b8-block-command-44" <<'PY'
import json
import pathlib
import sys

source = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
source.pop("ops")
source["client"] = {
    "gatewayEndpoint": sys.argv[4],
    "opsEndpoint": sys.argv[5],
    "scenarios": sys.argv[3],
    "streamTrace": sys.argv[6] == "1",
    "faultArmFile": sys.argv[7],
}
pathlib.Path(sys.argv[2]).write_text(json.dumps(source), encoding="utf-8")
PY
  printf '%s\n' "$path"
}

run_client() {
  local config
  config="$(client_config "$1")"
  "$CLIENT_BIN" --config "$config" 2>&1 | tee -a "$LOG_DIR/client.log"
  return "${PIPESTATUS[0]}"
}

next_log_line() {
  local path="$1"
  if [[ -f "$path" ]]; then
    echo $(( $(wc -l <"$path") + 1 ))
  else
    echo 1
  fi
}

# Runs a client scenario while the runner disrupts the topology underneath it.
run_client_with_stop() {
  local id="$1" node="$2" stop_kind="${3:-crash}" first_new_line client_pid armed_line
  if [[ "$SCENARIO" != "all" && "$SCENARIO" != *"$id"* ]]; then return 0; fi

  if [[ -f "$LOG_DIR/client.log" ]]; then
    first_new_line=$(($(wc -l <"$LOG_DIR/client.log") + 1))
  else
    first_new_line=1
  fi
  run_client "$id" &
  client_pid=$!
  # The client announces that the precondition it must observe is established before the
  # runner removes the node. A fixed delay can kill a loaded node before the client reaches
  # that state, turning a setup race into a false lifecycle failure.
  if ! wait_for_log_after client "scenario $id armed" "$first_new_line" 600; then
    wait "$client_pid" || status=1
    return 0
  fi
  if [[ "$node" == "auto" ]]; then
    armed_line="$(tail -n +"$first_new_line" "$LOG_DIR/client.log" \
      | grep -F "scenario $id armed node=" | tail -n 1)"
    node="${armed_line##*node=}"
    if [[ -z "${NODE_PID[$node]:-}" ]]; then
      echo "!! $id named an unknown node '$node'" >&2
      wait "$client_pid" || status=1
      status=1
      return 0
    fi
  fi
  if [[ "$stop_kind" == "graceful" ]]; then
    graceful_stop_node "$node"
  else
    stop_node "$node"
  fi
  wait "$client_pid" || status=1
  start_zone_node "$node"
}

wait_for_log() {
  local name="$1" pattern="$2" attempts="${3:-200}"
  for ((i = 0; i < attempts; i++)); do
    if grep -q "$pattern" "$LOG_DIR/$name.log" 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "!! $name never logged '$pattern'" >&2
  tail -20 "$LOG_DIR/$name.log" >&2 || true
  return 1
}

wait_for_log_after() {
  local name="$1" pattern="$2" first_line="$3" attempts="${4:-200}"
  for ((i = 0; i < attempts; i++)); do
    # Keep the bounded log wait compatible with `set -o pipefail`: grep -q can close its
    # process-substitution input as soon as it finds a match, so tail cannot turn a valid
    # readiness observation into a SIGPIPE false negative.
    if grep -q "$pattern" <(tail -n +"$first_line" "$LOG_DIR/$name.log" 2>/dev/null); then
      return 0
    fi
    sleep 0.1
  done
  echo "!! $name never logged '$pattern' after line $first_line" >&2
  tail -20 "$LOG_DIR/$name.log" >&2 || true
  return 1
}

wait_for_peer_admission_after() {
  local local_name="$1" local_rid="$2" local_first_line="$3"
  local peer_name="$4" peer_rid="$5" peer_first_line="$6"
  local attempts="${7:-200}"
  local local_pattern="mesh_peer_admission_accepted local=$local_rid peer=$peer_rid command=Admit"
  local peer_pattern="mesh_peer_admission_accepted local=$peer_rid peer=$local_rid command=Admit"

  # Either side may initiate the selected transport. Observe a guard-accepted admission rather
  # than a received descriptor, so the assertion proves that routed traffic can use the peer.
  for ((i = 0; i < attempts; i++)); do
    if grep -Fq "$local_pattern" <(tail -n +"$local_first_line" \
        "$LOG_DIR/$local_name.log" 2>/dev/null) \
      || grep -Fq "$peer_pattern" <(tail -n +"$peer_first_line" \
        "$LOG_DIR/$peer_name.log" 2>/dev/null); then
      return 0
    fi
    sleep 0.1
  done
  echo "!! neither side logged a completed mesh admission for $local_rid and $peer_rid" >&2
  tail -20 "$LOG_DIR/$local_name.log" >&2 || true
  tail -20 "$LOG_DIR/$peer_name.log" >&2 || true
  return 1
}

wait_for_zone_log() {
  local pattern="$1" attempts="${2:-200}"
  for ((i = 0; i < attempts; i++)); do
    if grep -q "$pattern" "$LOG_DIR"/zone-node-[12].log 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  echo "!! no zone owner logged '$pattern'" >&2
  tail -20 "$LOG_DIR"/zone-node-[12].log >&2 || true
  return 1
}

wait_for_file_while_running() {
  local path="$1" pid="$2" attempts="${3:-450}"
  for ((i = 0; i < attempts; i++)); do
    [[ -f "$path" ]] && return 0
    kill -0 "$pid" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

wait_for_b8_evidence_while_running() {
  local pattern="$1" pid="$2" attempts="${3:-600}"
  shift 3
  for ((i = 0; i < attempts; i++)); do
    if grep -Fq "$pattern" "$@" 2>/dev/null; then return 0; fi
    kill -0 "$pid" 2>/dev/null || return 1
    sleep 0.1
  done
  return 1
}

wait_for_port() {
  local port="$1"
  for _ in $(seq 1 200); do
    if (echo >/dev/tcp/127.0.0.1/"$port") >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for browser preview port $port." >&2
  return 1
}

echo "==> ops"
start ops "$OPS_BIN" --config "$CONFIG_DIR/ops.json"
wait_for_log ops "Application started."

echo "==> zone nodes"
# ZW-C4 needs a real spot runtime event, so zone-node-1's role configuration selects zone-nw
# as the one faulting tick. The other node configurations do not select a faulting zone.
# ZW-G2 starts the second application identity first. Each process receives an independent
# prefix-based RID, while the application NodeId remains unchanged.
if [[ "$B8_CHILD" == "1" ]]; then
  for proxy_name in zone-node-1 zone-node-2; do
    proxy_index="${proxy_name##*-}"
    proxy_port="${PORTS[$((proxy_index - 1))]}"
    start "session-route-proxy-$proxy_name" \
      python3 "$ROOT_DIR/Support/session_route_block_proxy.py" \
      --listen-host 127.0.0.1 --listen-port "$proxy_port" \
      --target-host 127.0.0.2 --target-port "$proxy_port" \
      --arm-file "$RUN_DIR/b8-block-command-44"
    wait_for_log "session-route-proxy-$proxy_name" "proxy-ready"
  done
fi
if scenario_selected ZW-G2 && [[ "$G4_CHILD" == "0" ]]; then
  start zone-node-2 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-2.json"
  start zone-node-1 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-1.json"
else
  start zone-node-1 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-1.json"
  start zone-node-2 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-2.json"
fi
wait_for_log zone-node-1 "topology=ready"
wait_for_log zone-node-2 "topology=ready"
wait_for_log ops "node status observed. node=zone-node-1, rid=zn-"
wait_for_log ops "node status observed. node=zone-node-2, rid=zn-"

G_RUNNER_LOG="$LOG_DIR/routing-id-self-check.log"
: >"$G_RUNNER_LOG"
g_pass() { echo "scenario $1 passed" | tee -a "$G_RUNNER_LOG"; }
g_fail() { echo "scenario $1 FAILED: $2" >&2; exit 1; }
if [[ "$G4_PROVEN" == "1" ]]; then g_pass ZW-G4; fi
if [[ "$B8_PROVEN" == "1" ]]; then g_pass ZW-B8; fi

node1_mesh_rid="$(routing_id_of zone-node-1)"
node2_mesh_rid="$(routing_id_of zone-node-2)"

# The common sample contract requires the two zone runtimes to establish their mesh peer before
# any scenario uses cross-node routing. topology=ready only proves local spot construction; this
# gate observes the completed admission on either side of the transport.
wait_for_peer_admission_after \
  zone-node-1 "$node1_mesh_rid" 1 \
  zone-node-2 "$node2_mesh_rid" 1 600

if scenario_selected ZW-G1 && [[ "$G4_CHILD" == "0" ]]; then
  if is_zone_node_rid "$node1_mesh_rid" \
      && is_zone_node_rid "$node2_mesh_rid" \
      && [[ "$node1_mesh_rid" != "$node2_mesh_rid" ]]; then
    g_pass ZW-G1
  else
    g_fail ZW-G1 "ZoneNode RIDs were not distinct canonical zn-UUIDv4 values"
  fi
fi
if scenario_selected ZW-G2 && [[ "$G4_CHILD" == "0" ]]; then
  if is_zone_node_rid "$node2_mesh_rid"; then
    g_pass ZW-G2-rid
  else
    g_fail ZW-G2 "reverse-started zone-node-2 did not publish a canonical zn-UUIDv4 RID"
  fi
fi

if scenario_selected ZW-G5; then
  set +e
  fixed_rid_hits="$(grep -R -nE \
    --include='*.cs' --include='*.json' --exclude-dir=bin --exclude-dir=obj \
    'SetRoutingId\(|(^|[^[:alnum:]])zn[12]([^[:alnum:]]|$)' \
    "$ROOT_DIR/Server/ZoneNode" "$ROOT_DIR/Server/Configuration" "$CONFIG_DIR" 2>&1)"
  fixed_rid_scan_status=$?
  set -e
  if [[ "$fixed_rid_scan_status" -eq 0 ]]; then
    g_fail ZW-G5 "fixed routing id found: $fixed_rid_hits"
  elif [[ "$fixed_rid_scan_status" -ne 1 ]]; then
    g_fail ZW-G5 "fixed routing-id scan failed: $fixed_rid_hits"
  fi
  g_pass ZW-G5
fi

# The third node hosts no zone: it only subscribes to the broadcast channel. It exists to
# show that Ops publishes without a node list — adding a node changes nothing on the
# publishing side (ZW-D2, scenario §11.1).
echo "==> zone-node-3 (fanout subscriber only)"
start zone-node-3 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-3.json"
wait_for_log zone-node-3 "topology=ready"

echo "==> gateway"
if [[ "$B8_CHILD" == "1" ]]; then
  start session-route-proxy-gateway \
    python3 "$ROOT_DIR/Support/session_route_block_proxy.py" \
    --listen-host 127.0.0.1 --listen-port "${PORTS[7]}" \
    --target-host 127.0.0.2 --target-port "${PORTS[7]}" \
    --arm-file "$RUN_DIR/b8-block-command-44"
  wait_for_log session-route-proxy-gateway "proxy-ready"
fi
start gateway "$GATEWAY_BIN" --config "$CONFIG_DIR/gateway.json"
wait_for_log gateway "Application started."
# topology=ready proves that each process created its local spots. Border synchronization has
# an additional distributed readiness boundary: each remote zone must receive at least one
# snapshot before a client can use cross-zone visibility as a deterministic assertion.
wait_for_zone_log "border subscription ready. zone=zone-nw, from=zone-ne"
wait_for_zone_log "border subscription ready. zone=zone-sw, from=zone-se"
wait_for_zone_log "border subscription ready. zone=zone-ne, from=zone-nw"
wait_for_zone_log "border subscription ready. zone=zone-se, from=zone-sw"

if [[ "$B8_CHILD" == "1" ]]; then
  first_b8_client_line="$(next_log_line "$LOG_DIR/client.log")"
  run_client ZW-B8 &
  b8_client_pid=$!
  if ! wait_for_log_after client "scenario ZW-B8 armed actor=" "$first_b8_client_line" 600; then
    wait "$b8_client_pid" || true
    g_fail ZW-B8 "client did not establish its initial bound session"
  fi
  b8_armed_line="$(tail -n +"$first_b8_client_line" "$LOG_DIR/client.log" \
    | grep -F "scenario ZW-B8 armed actor=" | tail -n 1)"
  b8_actor="${b8_armed_line#*actor=}"
  b8_actor="${b8_actor%% target=*}"
  b8_target="${b8_armed_line##* target=}"
  : >"$RUN_DIR/b8-block-command-44"
  if ! wait_for_b8_evidence_while_running \
      "blocked-command-44" "$b8_client_pid" 600 \
      "$LOG_DIR"/session-route-proxy-*.log; then
    wait "$b8_client_pid" || true
    g_fail ZW-B8 "precondition unmet: fault proxy did not intercept command 44"
  fi
  b8_commit_pattern="zone spot: player entered. zone=$b8_target, player=$b8_actor, bot=False, initial=False"
  if ! wait_for_b8_evidence_while_running \
      "$b8_commit_pattern" "$b8_client_pid" 600 \
      "$LOG_DIR"/zone-node-[12].log; then
    wait "$b8_client_pid" || true
    g_fail ZW-B8 \
      "precondition unmet: target relocation commit was not observed for actor $b8_actor in $b8_target"
  fi
  rm -f "$RUN_DIR/b8-block-command-44"
  if ! wait "$b8_client_pid"; then
    g_fail ZW-B8 "post-boundary reconnect did not rebind the existing relocated Actor"
  fi
  g_pass ZW-B8
  exit 0
fi

# G4 owns a fresh process/Store scope. Start an actual cross-owner Actor Join, crash its target
# after the client arms, require the source-side completion to surface Unavailable, then start a
# new process with the same NodeId and prove both its RID and fresh-object path.
if [[ "$G4_CHILD" == "1" ]]; then
  old_rid="$node2_mesh_rid"
  first_client_line="$(next_log_line "$LOG_DIR/client.log")"
  first_target_line="$(next_log_line "$LOG_DIR/zone-node-2.log")"
  run_client ZW-G4 &
  g4_client_pid=$!
  if ! wait_for_log_after client "scenario ZW-G4 armed node=zone-node-2" \
      "$first_client_line" 600; then
    wait "$g4_client_pid" || true
    g_fail ZW-G4 "crash-boundary client never armed"
  fi
  if ! wait_for_log_after zone-node-2 "crash-boundary join pending" \
      "$first_target_line" 600; then
    wait "$g4_client_pid" || true
    g_fail ZW-G4 "target owner never entered the in-flight crash boundary"
  fi
  stop_node zone-node-2
  if ! wait "$g4_client_pid"; then
    g_fail ZW-G4 "the interrupted operation did not terminate as Unavailable"
  fi
  first_replacement_ops_line="$(next_log_line "$LOG_DIR/ops.log")"
  # crash 교체는 이전 owner의 zone을 되찾지 않는다 — ZoneWorld 스펙 7.5는 "새 object를
  # 수용할 수 있게 되는 것"이지 "이전 Ready owner가 소유하던 object의 자동 복원·재생성이
  # 아니다"라고 정한다. 그래서 zone 0개로 ready가 되는 전용 config를 쓴다.
  start_zone_node zone-node-2 zone-node-crash-replacement
  crash_rid="$(routing_id_of zone-node-2 "$first_replacement_ops_line")"
  if ! is_zone_node_rid "$crash_rid" || [[ "$crash_rid" == "$old_rid" ]]; then
    g_fail ZW-G4 "crash replacement did not publish a new canonical zn-UUIDv4 RID"
  fi
  first_fresh_line="$(next_log_line "$LOG_DIR/client.log")"
  if ! run_client ZW-G4-fresh; then
    g_fail ZW-G4 "crash replacement fresh-object probe failed"
  fi
  if ! tail -n +"$first_fresh_line" "$LOG_DIR/client.log" \
      | grep -Fq "scenario ZW-G4-fresh owner=$crash_rid "; then
    g_fail ZW-G4 "no fresh Actor was placed on the crash replacement RID"
  fi
  g_pass ZW-G4
  exit 0
fi

if [[ "$SCENARIO" == "all" || "$SCENARIO" == *"ZW-G2"* ]]; then
  run_client ZW-G2
fi

echo "==> scenarios ($SCENARIO)"
# The same browser client is shared by every language implementation. Language runners opt in
# to the live browser smoke with one flag; the client receives only Gateway and Ops endpoints.
if [[ "$BROWSER_SMOKE" == "1" ]]; then
  echo "==> shared browser client"
  browser_client="$ROOT_DIR/../../../shared_sample/zoneworld/client"
  browser_dist="$RUN_DIR/browser-dist"
  browser_marker="$RUN_DIR/browser-lifecycle-armed"
  browser_config="$RUN_DIR/playwright.live.config.mjs"
  (cd "$browser_client" && npm exec vite build -- --outDir "$browser_dist")
  python3 - "$browser_dist/config.json" "$GATEWAY_ENDPOINT" "$OPS_ENDPOINT" \
    "$browser_config" "$browser_client/tests/live" "$BROWSER_PREVIEW_PORT" "$browser_marker" <<'PY'
import json
import pathlib
import sys

config_path, gateway, ops, playwright_path, test_dir, port, marker = sys.argv[1:]
pathlib.Path(config_path).write_text(
    json.dumps({"gateway": gateway, "ops": ops}), encoding="utf-8")
pathlib.Path(playwright_path).write_text(
    "export default " + json.dumps({
        "testDir": test_dir,
        "timeout": 45_000,
        "workers": 1,
        "use": {"baseURL": f"http://127.0.0.1:{port}", "headless": True},
        "metadata": {
            "lifecycleMarker": marker,
            "lifecycleNodeId": "zone-node-2",
        },
    }), encoding="utf-8")
PY
  (cd "$browser_client" && npm exec vite preview -- \
    --host 127.0.0.1 --port "$BROWSER_PREVIEW_PORT" --outDir "$browser_dist" \
    >"$LOG_DIR/browser-preview.stdout.log" 2>"$LOG_DIR/browser-preview.stderr.log") &
  preview_pid=$!
  PIDS+=("$preview_pid")
  wait_for_port "$BROWSER_PREVIEW_PORT"
  (cd "$browser_client" && npm exec playwright test -- --config "$browser_config") &
  browser_pid=$!
  browser_node_stopped=0

  if wait_for_file_while_running "$browser_marker" "$browser_pid"; then
    stop_node zone-node-2
    browser_node_stopped=1
  else
    echo "!! browser client did not arm its node lifecycle check" >&2
  fi

  set +e
  wait "$browser_pid"
  browser_status=$?
  set -e
  if [[ "$browser_node_stopped" -eq 1 ]]; then
    start_zone_node zone-node-2
  else
    browser_status=1
  fi
  if [[ "$browser_status" -ne 0 ]]; then
    echo "!! shared browser client failed" >&2
    exit "$browser_status"
  fi
fi
set +e
CLIENT_SCENARIO="$(printf '%s' "$SCENARIO" | tr ',' '\n' \
  | grep -vxE 'ZW-D2|ZW-F2|ZW-C2|ZW-C3|ZW-B4|ZW-E5|ZW-E5-arm|ZW-G[1-5]' \
  | paste -sd, -)"
if [[ "$SCENARIO" == "all" || -n "$CLIENT_SCENARIO" ]]; then
  CLIENT_CONFIG="$(client_config "${CLIENT_SCENARIO:-$SCENARIO}")"
  "$CLIENT_BIN" --config "$CLIENT_CONFIG" 2>&1 | tee -a "$LOG_DIR/client.log"
  status=${PIPESTATUS[0]}
else
  status=0
fi
set -e

# Some of what §11 asks for is not observable from a client: the absence of a client (ZW-F2), a
# node the client is never told about (ZW-D2), another node's fanout subscriber (ZW-D1), the
# whole bot population (ZW-F1), or a push that must never be attempted (ZW-F3). The runner reads
# those out of the server logs.
# The client writes its verdicts to client.log; these write theirs here. The §12 markers below
# are decided from both, so a runner verdict has to be recorded, not just printed.
RUNNER_LOG="$LOG_DIR/runner.log"
: >"$RUNNER_LOG"
cat "$G_RUNNER_LOG" >>"$RUNNER_LOG"

pass() { echo "scenario $1 passed" | tee -a "$RUNNER_LOG"; }
fail() { echo "scenario $1 FAILED: $2" >&2; echo "scenario $1 failed" >>"$RUNNER_LOG"; status=1; }

# A runner verdict such as ZW-D1-spots belongs to ZW-D1: it asserts the half of that scenario a
# client cannot see. Selecting ZW-D1 has to select it too, or a targeted run quietly proves less
# than the same scenario proves in a full one.
selects() {
  [[ "$SCENARIO" == "all" ]] && return 0
  local base
  base="$(printf '%s' "$1" | cut -d- -f1,2)"
  [[ "$SCENARIO" == *"$base"* ]]
}

# ZW-B5 is one-way and ZW-B6 is request/reply. Each primes the old owner independently, so each
# must produce exactly one committed-target handler and exactly one source Follow relay.
if selects ZW-B5; then
  b5_line="$(grep -F 'message-follow-one-way completed ' "$LOG_DIR/client.log" | tail -n 1 || true)"
  actor_id=""
  one_way_id=""
  if [[ "$b5_line" =~ actor=([^[:space:]]+)[[:space:]]probe=([^[:space:]]+) ]]; then
    actor_id="${BASH_REMATCH[1]}"
    one_way_id="${BASH_REMATCH[2]}"
  fi
  one_way_payload="$(printf '%s' 'one-way-payload' | od -An -tx1 -v \
    | tr -d ' \\n' | tr '[:lower:]' '[:upper:]')"
  one_way_hits="$(awk -v actor="$actor_id" -v probe="$one_way_id" -v payload="$one_way_payload" \
    '/message-follow probe one-way handled\./ \
      && index($0, "actor=" actor ",") > 0 \
      && index($0, "probe=" probe ",") > 0 \
      && index($0, "payload=" payload) > 0 { count++ } \
    END { print count + 0 }' "$LOG_DIR"/zone-node-*.log 2>/dev/null)"
  relay_hits="$(awk -v actor="$actor_id" \
    '/message_follow_relay/ && index($0, "actor=" actor) > 0 { count++ } \
    END { print count + 0 }' "$LOG_DIR"/zone-node-*.log 2>/dev/null)"
  if [[ -n "$actor_id" && -n "$one_way_id" \
      && "$one_way_hits" -eq 1 && "$relay_hits" -eq 1 ]]; then
    pass ZW-B5
  else
    fail ZW-B5 "one-way Follow evidence was incomplete (actor=$actor_id handler=$one_way_hits relay=$relay_hits)"
  fi
fi

if selects ZW-B6; then
  b6_line="$(grep -F 'message-follow-request completed ' "$LOG_DIR/client.log" | tail -n 1 || true)"
  actor_id=""
  request_id=""
  if [[ "$b6_line" =~ actor=([^[:space:]]+)[[:space:]]request=([^[:space:]]+) ]]; then
    actor_id="${BASH_REMATCH[1]}"
    request_id="${BASH_REMATCH[2]}"
  fi
  request_payload="$(printf '%s' 'request-payload' | od -An -tx1 -v \
    | tr -d ' \\n' | tr '[:lower:]' '[:upper:]')"
  request_hits="$(awk -v actor="$actor_id" -v probe="$request_id" -v payload="$request_payload" \
    '/message-follow probe handled\./ \
      && index($0, "actor=" actor ",") > 0 \
      && index($0, "probe=" probe ",") > 0 \
      && index($0, "payload=" payload) > 0 { count++ } \
    END { print count + 0 }' "$LOG_DIR"/zone-node-*.log 2>/dev/null)"
  relay_hits="$(awk -v actor="$actor_id" \
    '/message_follow_relay/ && index($0, "actor=" actor) > 0 { count++ } \
    END { print count + 0 }' "$LOG_DIR"/zone-node-*.log 2>/dev/null)"
  if [[ -n "$actor_id" && -n "$request_id" \
      && "$request_hits" -eq 1 && "$relay_hits" -eq 1 ]]; then
    pass ZW-B6
  else
    fail ZW-B6 "request Follow evidence was incomplete (actor=$actor_id handler=$request_hits relay=$relay_hits)"
  fi
fi

runner_scenario() {
  local id="$1" description="$2" log="$3" pattern="$4"
  selects "$id" || return 0
  if grep -q "$pattern" "$LOG_DIR/$log" 2>/dev/null; then
    pass "$id"
  else
    fail "$id" "$description"
  fi
}

# Passes only when each log contains its own half of the same cross-node scenario.
runner_scenario_pair() {
  local id="$1" description="$2"
  selects "$id" || return 0

  if [[ "$id" == "ZW-F2" ]]; then
    # The fixed bot id is not a contract: all four X-axis bots are valid witnesses, and
    # capacity placement can initially put either side on either process. A non-initial
    # entry is the target-side completion of an Actor Join. Seeing the same bot complete
    # one on both process logs proves it crossed the process-owner boundary in both
    # directions without depending on an internal debug-only source-leave marker.
    local actor
    for _ in $(seq 1 600); do
      while IFS= read -r actor; do
        [[ -n "$actor" ]] || continue
        if grep -Fq "player=$actor, bot=True, initial=False" \
            "$LOG_DIR/zone-node-2.log" 2>/dev/null; then
          pass "$id"
          return 0
        fi
      done < <(sed -nE \
        's/.*player=(bot-[^,]+), bot=True, initial=False.*/\1/p' \
        "$LOG_DIR/zone-node-1.log" 2>/dev/null | sort -u)
      sleep 0.1
    done
    fail "$id" "$description (no correlated cross-node bot handoff)"
    return 0
  fi

  local first_log="$3" first_pattern="$4"
  local second_log="$5" second_pattern="$6"
  # A selected F2 run starts a fresh world and has no client traffic to keep the patrol
  # advancing. Wait for the same process evidence that a full run observes later; a fixed
  # sleep would make the assertion depend on machine speed.
  if ! wait_for_log "${first_log%.log}" "$first_pattern" 600; then
    fail "$id" "$description ($first_log)"
    return 0
  fi
  if ! wait_for_log "${second_log%.log}" "$second_pattern" 600; then
    fail "$id" "$description ($second_log)"
    return 0
  fi
  pass "$id"
}

# Passes only when the pattern appears in *every* named log.
runner_scenario_all() {
  local id="$1" description="$2" pattern="$3"; shift 3
  selects "$id" || return 0
  local log
  for log in "$@"; do
    if ! grep -q "$pattern" "$LOG_DIR/$log" 2>/dev/null; then
      fail "$id" "$description ($log)"
      return 0
    fi
  done
  pass "$id"
}

# Passes only when the pattern appears nowhere. An assertion about something that must not
# happen has to be written as an absence, or it asserts nothing.
runner_scenario_absent() {
  local id="$1" description="$2" pattern="$3"; shift 3
  selects "$id" || return 0
  local hits
  hits="$(grep -l "$pattern" "${@/#/$LOG_DIR/}" 2>/dev/null || true)"
  if [[ -n "$hits" ]]; then
    fail "$id" "$description ($hits)"
  else
    pass "$id"
  fi
}

# These all need zone-node-2 to disappear while a client is watching, and each one restarts it
# afterwards. ZW-B4 goes first because it is the only one that has to *use* zone-node-2 before
# taking it away — it walks a player into zone-ne — and a node that has just come back is the
# least reliable thing to walk into.
run_client_with_stop ZW-B4 auto crash
run_client_with_stop ZW-C2 zone-node-2 graceful
run_client_with_stop ZW-C3 zone-node-2 crash

# ZW-E5: the operator closes a node, the node restarts, and it comes back still closed —
# the desired state lives in the store, not in a message.
if [[ "$SCENARIO" == "all" || "$SCENARIO" == *"ZW-E5"* ]]; then
  run_client ZW-E5-arm || status=1
  stop_node zone-node-2
  start_zone_node zone-node-2
  run_client ZW-E5 || status=1
fi

# ZW-D1: one publish, no node list, and it comes out of *both* nodes' fanout subscribers and
# reaches *every* zone spot. The client half of ZW-D1 asserts what a client can see — that an
# announcement it receives is never a duplicate — so these carry their own ids: a client verdict
# and a runner verdict on the same id would be indistinguishable in the log.
runner_scenario_all ZW-D1-subscribers "a node's fanout subscriber never received the announcement" \
  "fanout subscriber received announcement" zone-node-1.log zone-node-2.log
runner_scenario_all ZW-D1-spots "a zone spot never received the announcement" \
  "zone spot: announcement delivered" zone-node-1.log zone-node-2.log

# ZW-F1: the world holds eight bots. No client sees them all — a client only ever receives one
# zone's view — so the population is counted here.
if selects ZW-F1-population; then
  bots="$(grep -ho "bot spawned. bot=[a-z0-9-]*" "$LOG_DIR"/zone-node-*.log 2>/dev/null \
    | sort -u | wc -l)"
  expected_bots=(
    'bot=bot-nw-x, zone=zone-nw, start=(10,15), dir=(1,0)'
    'bot=bot-nw-y, zone=zone-nw, start=(15,10), dir=(0,1)'
    'bot=bot-ne-x, zone=zone-ne, start=(90,15), dir=(-1,0)'
    'bot=bot-ne-y, zone=zone-ne, start=(85,10), dir=(0,1)'
    'bot=bot-sw-x, zone=zone-sw, start=(10,85), dir=(1,0)'
    'bot=bot-sw-y, zone=zone-sw, start=(15,90), dir=(0,-1)'
    'bot=bot-se-x, zone=zone-se, start=(90,85), dir=(-1,0)'
    'bot=bot-se-y, zone=zone-se, start=(85,90), dir=(0,-1)'
  )
  fixed_roster=1
  for expected in "${expected_bots[@]}"; do
    if ! grep -Fq "$expected" "$LOG_DIR"/zone-node-*.log 2>/dev/null; then
      fixed_roster=0
      break
    fi
  done
  if [[ "$bots" -eq 8 && "$fixed_roster" -eq 1 ]]; then
    pass ZW-F1-population
  else
    fail ZW-F1-population "the fixed eight-bot roster/initial trajectory was not observed (count=$bots)"
  fi
fi

# ZW-F3: a bot has no bound session, so nothing is ever pushed to one (§11 ZW-F3). That is an
# absence, asserted as one — a push to a bot would leave this error behind with the bot's id.
#
# The id matters. The same error also fires, benignly, for a *human* whose client disconnected in
# the tick before the spot dropped them: the spot catches it and moves on by design (§8.9.2, and
# ZoneSpot.PushToClients). That is a different, known case — not what ZW-F3 is about — so the
# check names bot ids ("bot-...") only. If the bot exclusion ever broke, the push would carry a
# bot id and this would catch it; a human mid-disconnect does not.
runner_scenario_absent ZW-F4-no-push "a push was attempted to a bot (an actor with no bound session)" \
  "No current session binding exists for actor 'bot-" \
  zone-node-1.log zone-node-2.log zone-node-3.log

# ZW-D2: the third node received the announcement with no change to Ops.
runner_scenario ZW-D2 "zone-node-3 never received a world announcement" \
  zone-node-3.log "fanout subscriber received announcement"

# ZW-F2: a bot crossed to the other node. The bots run with no client attached, so a
# relocation recorded here proves the actor moved without a bound session.
runner_scenario_pair ZW-F2 "no bot relocated across nodes" \
  "" "" "" ""

# ZW-G3 replaces a normally stopped node. Run this destructive ownership check after every
# normal topology scenario so the replacement cannot become an accidental dependency of the
# sample behavior that G3 is supposed to be independent from.
if scenario_selected ZW-G3 && [[ "$G4_CHILD" == "0" ]]; then
  old_rid="$node2_mesh_rid"
  graceful_stop_node zone-node-2
  first_replacement_ops_line=$(($(wc -l <"$LOG_DIR/ops.log") + 1))
  start zone-node-replacement "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-replacement.json"
  wait_for_log zone-node-replacement "topology=ready" 600
  wait_for_log_after ops "node status observed. node=zone-node-2, rid=zn-" \
    "$first_replacement_ops_line" 600
  replacement_rid="$(routing_id_of zone-node-2 "$first_replacement_ops_line")"
  if is_zone_node_rid "$replacement_rid" && [[ "$replacement_rid" != "$old_rid" ]] \
      && run_client ZW-A1; then
    g_pass ZW-G3
  else
    g_fail ZW-G3 "normal replacement did not publish a new RID and accept a fresh object"
  fi
fi

# §12 asks the sample to say what it proved, not just that it exited zero. The runner owns these
# markers because no single client run sees the whole suite: the client's batch leaves out the
# scenarios that need a node taken away, and six more are judged from server logs. A marker is
# printed only when every scenario behind it actually passed — a phase that says "completed"
# without having run is worse than no marker at all.
declare -A PASSED=()
while read -r id; do PASSED["$id"]=1; done < <(
  sed -nE 's/^scenario ([A-Za-z0-9-]+) passed$/\1/p' \
    "$LOG_DIR/client.log" "$RUNNER_LOG" "$G_RUNNER_LOG" 2>/dev/null
)

phase() {
  local marker="$1"; shift
  local id
  if [[ "$status" -ne 0 ]]; then
    echo "!! $marker withheld: at least one selected verdict failed" >&2
    return 1
  fi
  for id in "$@"; do
    if [[ -z "${PASSED[$id]:-}" ]]; then
      echo "!! $marker withheld: $id did not pass" >&2
      status=1
      return 1
    fi
  done
  echo "$marker"
}

# Only a full run can claim a phase. A selective run proves one scenario, not a capability.
if [[ "$SCENARIO" == "all" ]]; then
  phase "zoneworld-relocation=completed"      ZW-B2 ZW-B3 ZW-B5 ZW-B6 ZW-B7 ZW-B8 ZW-F2
  phase "zoneworld-border-sync=completed"     ZW-B1 ZW-B4
  phase "zoneworld-ops-observe=completed"     ZW-C1 ZW-C2 ZW-C3 ZW-C4
  phase "zoneworld-ops-announce=completed"    ZW-D1 ZW-D1-subscribers ZW-D1-spots ZW-D2
  phase "zoneworld-ops-maintenance=completed" ZW-E1 ZW-E2 ZW-E3 ZW-E4 ZW-E5 ZW-E6

  # The whole of §11, plus the six verdicts only the runner can reach. Gated on status as well:
  # a scenario can fail without its id ever reaching the log.
  phase "zoneworld=completed" \
    ZW-A1 ZW-A2 ZW-A3 ZW-A4 ZW-A5 \
    ZW-B1 ZW-B2 ZW-B3 ZW-B4 ZW-B5 ZW-B6 ZW-B7 ZW-B8 \
    ZW-C1 ZW-C2 ZW-C3 ZW-C4 \
    ZW-D1 ZW-D1-subscribers ZW-D1-spots ZW-D2 \
    ZW-E1 ZW-E2 ZW-E3 ZW-E4 ZW-E5 ZW-E5-arm ZW-E6 \
    ZW-F1 ZW-F1-population ZW-F2 ZW-F3 ZW-F4 ZW-F4-no-push \
    ZW-G1 ZW-G2-rid ZW-G2 ZW-G3 ZW-G4 ZW-G5
fi

echo "==> logs: $LOG_DIR"
exit "$status"
