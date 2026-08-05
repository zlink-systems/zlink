#!/usr/bin/env bash
# Config 11 — ObservabilityOps (metrics · flow correlation · drain).
# Scenario subsets are asserted; incomplete OBS ids remain deferred with their
# blocking work recorded in feature-map.ko.md.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRAMEWORK_DIR="$(cd "$ROOT_DIR/../.." && pwd)"
source "$ROOT_DIR/../redis-common.sh"
BUILD_DIR="$FRAMEWORK_DIR/build"
SCENARIO="${1:-all}"
LOCAL_READINESS_TIMEOUT_SECONDS=3
LOCAL_READINESS_POLL_SECONDS=0.1
ROUTE_SETTLE_SECONDS=5
SCENARIO_SETTLE_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
EVIDENCE_POLL_SECONDS=0.2

RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/logs/$RUN_ID"
CONFIG_DIR="$LOG_DIR/config"
mkdir -p "$LOG_DIR"
mkdir -p "$CONFIG_DIR"
echo "log_dir=$LOG_DIR"

assign_ports() {
  read -r PLAY_A_HTTP PLAY_B_HTTP SESSION_HTTP WORKFLOW_A_HTTP WORKFLOW_B_HTTP \
    PLAY_A_ROUTE PLAY_B_ROUTE SESSION_ROUTE WORKFLOW_A_ROUTE WORKFLOW_B_ROUTE \
    PLAY_A_SPOT_ROUTER PLAY_B_SPOT_ROUTER SESSION_SPOT_ROUTER \
    WORKFLOW_A_SPOT_ROUTER WORKFLOW_B_SPOT_ROUTER \
    PLAY_A_SPOT_PUB PLAY_B_SPOT_PUB SESSION_SPOT_PUB \
    WORKFLOW_A_SPOT_PUB WORKFLOW_B_SPOT_PUB \
    STREAM_ENDPOINT <<<"$(python3 - <<'PY'
import socket
import secrets
sockets = []
ports = []
# Do not ask the kernel's port-0 allocator for listener candidates. The same
# allocator immediately chooses outbound local ports while roles start
# sequentially, which can reclaim a later role's not-yet-bound candidate.
candidates = list(range(10000, 30000))
secrets.SystemRandom().shuffle(candidates)
for port in candidates:
    try:
        s = socket.socket()
        s.bind(("127.0.0.1", port))
    except OSError:
        s.close()
        continue
    sockets.append(s)
    ports.append(port)
    if len(ports) == 21:
        break
if len(ports) != 21:
    raise SystemExit("cannot reserve 21 listener ports")
values = [f"http://127.0.0.1:{p}" for p in ports[:5]]
values += [f"tcp://127.0.0.1:{p}" for p in ports[5:21]]
print(" ".join(values))
for s in sockets:
    s.close()
PY
)"
}
assign_ports

cmake -S "$FRAMEWORK_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target \
  zlink_cpp_e2e_observability_ops_session \
  zlink_cpp_e2e_observability_ops_play \
  zlink_cpp_e2e_observability_ops_order_workflow \
  zlink_cpp_e2e_observability_ops_client >/dev/null

SESSION_SERVER="$BUILD_DIR/zlink_cpp_e2e_observability_ops_session"
PLAY_SERVER="$BUILD_DIR/zlink_cpp_e2e_observability_ops_play"
ORDER_WORKFLOW_SERVER="$BUILD_DIR/zlink_cpp_e2e_observability_ops_order_workflow"
CLIENT="$BUILD_DIR/zlink_cpp_e2e_observability_ops_client"

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-observabilityops" "redis:7-alpine"
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
REDIS_KEY_PREFIX="zlink:cpp:observability-ops:${RUN_ID}"
PIDS=()

cleanup() {
  local code=$?
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then kill "$pid" >/dev/null 2>&1 || true; fi
  done
  for pid in "${PIDS[@]:-}"; do wait "$pid" 2>/dev/null || true; done
  docker rm -fv "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  rm -rf "$CONFIG_DIR"
  if [[ "$code" -ne 0 ]]; then echo "ObservabilityOps failed. Logs: $LOG_DIR" >&2; fi
}
trap cleanup EXIT

launch_role() {
  local binary="$1" node_rid="$2" http="$3" route="$4" peer_route="$5" spot_router="$6" spot_pub="$7" stream="$8"
  local log_dir="${9:-$LOG_DIR}"
  local trace_mode="${10:-key_transitions}"
  local metrics="${11:-on}"
  local room_timer="${12:-off}"
  local config_path="$CONFIG_DIR/$node_rid-$(date +%s%N).json"
  mkdir -p "$log_dir"
  python3 - "$config_path" "$node_rid" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" \
    "$http" "$route" "$peer_route" "$spot_router" "$spot_pub" "$stream" \
    "$log_dir" "$trace_mode" "$metrics" "$room_timer" <<'PY'
import json
import os
import stat
import sys

(path, node_rid, redis_endpoint, redis_key_prefix, http, route, peer_route,
 spot_router, spot_pub, stream, log_dir, trace_mode, metrics, room_timer) = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"nodeRid": node_rid,
        "redis": {"endpoint": redis_endpoint, "keyPrefix": redis_key_prefix},
        "httpEndpoint": http, "routeEndpoint": route,
        "peerRouteEndpoint": peer_route, "spotRouterEndpoint": spot_router,
        "spotPubEndpoint": spot_pub, "streamEndpoint": stream,
        "logDir": log_dir, "traceMode": trace_mode, "metrics": metrics,
        "roomTimer": room_timer}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$binary" --config="$config_path" \
    >"$log_dir/$node_rid.stdout.log" 2>"$log_dir/$node_rid.stderr.log" &
  PIDS+=("$!")
}

stop_roles() {
  for pid in "${PIDS[@]:-}"; do
    if kill -0 "$pid" >/dev/null 2>&1; then kill "$pid" >/dev/null 2>&1 || true; fi
  done
  for pid in "${PIDS[@]:-}"; do wait "$pid" 2>/dev/null || true; done
  PIDS=()
}

curl_local() {
  curl --max-time "$HTTP_PROBE_TIMEOUT_SECONDS" \
    --connect-timeout "$HTTP_PROBE_TIMEOUT_SECONDS" "$@"
}

wait_health() {
  local url="$1"
  local attempts
  attempts="$(python3 - "$LOCAL_READINESS_TIMEOUT_SECONDS" "$LOCAL_READINESS_POLL_SECONDS" <<'PY'
import math
import sys
print(max(1, math.ceil(float(sys.argv[1]) / float(sys.argv[2]))))
PY
)"
  for _ in $(seq 1 "$attempts"); do
    if curl_local -fsS "$url/health" >/dev/null 2>&1; then return 0; fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  echo "Timed out waiting ${LOCAL_READINESS_TIMEOUT_SECONDS}s for $url" >&2
  return 1
}

launch_role "$SESSION_SERVER" session "$SESSION_HTTP" "$SESSION_ROUTE" "$PLAY_A_ROUTE" \
  "$SESSION_SPOT_ROUTER" "$SESSION_SPOT_PUB" "$STREAM_ENDPOINT" \
  "$LOG_DIR" key_transitions on off
wait_health "$SESSION_HTTP"
launch_role "$PLAY_SERVER" play-a "$PLAY_A_HTTP" "$PLAY_A_ROUTE" "$PLAY_B_ROUTE" \
  "$PLAY_A_SPOT_ROUTER" "$PLAY_A_SPOT_PUB" "" \
  "$LOG_DIR" key_transitions on on
wait_health "$PLAY_A_HTTP"
launch_role "$PLAY_SERVER" play-b "$PLAY_B_HTTP" "$PLAY_B_ROUTE" "$PLAY_A_ROUTE" \
  "$PLAY_B_SPOT_ROUTER" "$PLAY_B_SPOT_PUB" ""
wait_health "$PLAY_B_HTTP"
launch_role "$ORDER_WORKFLOW_SERVER" workflow-a "$WORKFLOW_A_HTTP" "$WORKFLOW_A_ROUTE" \
  "$WORKFLOW_B_ROUTE" "$WORKFLOW_A_SPOT_ROUTER" "$WORKFLOW_A_SPOT_PUB" ""
wait_health "$WORKFLOW_A_HTTP"
launch_role "$ORDER_WORKFLOW_SERVER" workflow-b "$WORKFLOW_B_HTTP" "$WORKFLOW_B_ROUTE" \
  "$WORKFLOW_A_ROUTE" "$WORKFLOW_B_SPOT_ROUTER" "$WORKFLOW_B_SPOT_PUB" ""
wait_health "$WORKFLOW_B_HTTP"
sleep "$ROUTE_SETTLE_SECONDS"

ensure() {
  local condition_result="$1" message="$2"
  if [[ "$condition_result" != "0" ]]; then
    echo "ensure failed: $message" >&2
    exit 1
  fi
}

write_trigger_config() {
  local path="$1" scenario="$2" spot_id="$3"
  python3 - "$path" "$STREAM_ENDPOINT" "$scenario" "$spot_id" <<'PY'
import json
import os
import stat
import sys

path, stream_endpoint, scenario, spot_id = sys.argv[1:]
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"streamEndpoint": stream_endpoint,
        "scenario": scenario, "spotId": spot_id}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

verify_scenario() {
  local scenario_id="$1"
  shift
  local path="$CONFIG_DIR/verify-${scenario_id,,}-$(date +%s%N).json"
  python3 - "$path" "$scenario_id" "$@" <<'PY'
import json
import os
import stat
import sys

path, scenario_id, *pairs = sys.argv[1:]
if len(pairs) % 2:
    raise SystemExit(f"{scenario_id} verification files must be key/path pairs")
files = {pairs[index]: pairs[index + 1] for index in range(0, len(pairs), 2)}
with open(path, "w", encoding="utf-8") as file:
    json.dump({"e2e": {"verification": {
        "scenarioId": scenario_id, "files": files}}}, file, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
  "$CLIENT" --config="$path"
}

SPOT_ID="obs-room-1"
WORKFLOW_SPOT_ID="obs-workflow-1"
curl_local -fsS -X POST "$PLAY_B_HTTP/spot/create" -H 'Content-Type: application/json' \
  -d "{\"spotId\":\"$SPOT_ID\"}" >"$LOG_DIR/create-room.json"
python3 - "$LOG_DIR/create-room.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
assert body["state"] in ("created", "existing"), body
PY
sleep "$ROUTE_SETTLE_SECONDS"

if [[ "$SCENARIO" == "all" || "$SCENARIO" == "flow" ]]; then
  # OBS-A1 — one connector-generated flow id threads
  # trigger -> play-a session inbound -> play-b room-spot dispatch.
  write_trigger_config "$CONFIG_DIR/trigger-flow.json" flow "$SPOT_ID"
  "$CLIENT" --config="$CONFIG_DIR/trigger-flow.json" >"$LOG_DIR/trigger-flow.log" 2>&1
  verify_scenario OBS-A1 sessionLog "$LOG_DIR/session-flow.log" \
    spotLog "$LOG_DIR/play-b-flow.log"

  # OBS-A2 — the dispatch error line carries flow= too.
  write_trigger_config "$CONFIG_DIR/trigger-error.json" error "$SPOT_ID"
  "$CLIENT" --config="$CONFIG_DIR/trigger-error.json" >"$LOG_DIR/trigger-error.log" 2>&1
  verify_scenario OBS-A2 sessionLog "$LOG_DIR/session-flow.log"
fi

if [[ "$SCENARIO" == "all" || "$SCENARIO" == "metrics" ]]; then
  # OBS-B subset — spot.created/spot.count with kind label and
  # channel.request.duration samples appear in the evidence collector.
  # The aggregate run has already applied the flow action to this room. Use
  # the accumulating action assertion so the metrics probe does not depend on
  # whether it runs alone or after OBS-A1.
  write_trigger_config "$CONFIG_DIR/trigger-metrics.json" fanout "$SPOT_ID"
  "$CLIENT" --config="$CONFIG_DIR/trigger-metrics.json" >"$LOG_DIR/trigger-metrics.log" 2>&1
  write_trigger_config "$CONFIG_DIR/trigger-metrics-error.json" error "$SPOT_ID"
  "$CLIENT" --config="$CONFIG_DIR/trigger-metrics-error.json" >"$LOG_DIR/trigger-metrics-error.log" 2>&1
  curl_local -fsS "$PLAY_B_HTTP/evidence" >"$LOG_DIR/play-b.metrics.evidence.json"
  curl_local -fsS "$PLAY_A_HTTP/evidence" >"$LOG_DIR/play-a.metrics.evidence.json"
  curl_local -fsS "$SESSION_HTTP/evidence" >"$LOG_DIR/session.metrics.evidence.json"
  python3 - "$LOG_DIR/play-b.metrics.evidence.json" "$LOG_DIR/play-a.metrics.evidence.json" \
    "$LOG_DIR/session.metrics.evidence.json" <<'PY'
import json, sys
play_b = json.load(open(sys.argv[1], encoding="utf-8"))["metrics"]
created = [m for m in play_b if m["name"] == "zlink.spot.created" and m["kind"] == "counter"]
count = [m for m in play_b if m["name"] == "zlink.spot.count" and m["kind"] == "updown"]
assert created and created[0]["tags"].get("kind") in ("user", "entry"), created
assert count, "zlink.spot.count missing"
play_a = json.load(open(sys.argv[2], encoding="utf-8"))["metrics"]
session = json.load(open(sys.argv[3], encoding="utf-8"))["metrics"]
durations = [m for m in session if m["name"] == "zlink.channel.request.duration"]
assert durations and durations[0]["kind"] == "histogram" and durations[0]["unit"] == "s", durations
# OBS-B1 subset — server-side session counters follow accepts/closes.
opened = [m for m in session if m["name"] == "zlink.stream.connections.opened"]
closed = [m for m in session if m["name"] == "zlink.stream.connections.closed"]
active = [m for m in session if m["name"] == "zlink.stream.connections.active"]
assert len(opened) >= 2 and len(active) >= 2, (opened, active)
assert closed and all(
    m["tags"].get("close_reason") in ("client_close", "idle_timeout", "heartbeat_timeout",
                                      "server_drain", "protocol_error", "transport_error")
    for m in closed), closed
net_active = sum(m["value"] for m in active)
assert net_active == 0, f"active sessions should net to zero after triggers: {net_active}"
forbidden = {"correlation_id", "flow_id", "actor_id", "spot_id"}
for sample in play_a + play_b + session:
    assert not (forbidden & set(sample["tags"])), f"high-cardinality label: {sample}"
print("OBS-B(subset) PASS (spot/channel/stream instruments, closed labels)")
PY
  verify_scenario OBS-B1 sessionEvidence "$LOG_DIR/session.metrics.evidence.json"
fi

if [[ "$SCENARIO" == "all" || "$SCENARIO" == "fanout" ]]; then
  # OBS-A4 — one action fans out to every mesh subscriber under the same
  # flow id; a timer-originated publish starts a fresh flow (origin=timer).
  # OBS-B3 — fanout.published/received counters with the closed topic label.
  curl_local -fsS -X POST "$WORKFLOW_A_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d '{"spotId":"obs-room-sub"}' >"$LOG_DIR/create-room-sub.json"
  curl_local -fsS -X POST "$PLAY_A_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d '{"spotId":"obs-timer-room"}' >"$LOG_DIR/create-timer-room.json"
  curl_local -fsS -X POST "$WORKFLOW_B_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$WORKFLOW_SPOT_ID\"}" >"$LOG_DIR/create-workflow.json"
  sleep "$ROUTE_SETTLE_SECONDS"
  curl_local -fsS "$WORKFLOW_B_HTTP/evidence" >"$LOG_DIR/workflow-b.fanout.before.json"
  curl_local -fsS "$WORKFLOW_A_HTTP/evidence" >"$LOG_DIR/workflow-a.fanout.before.json"
  curl_local -fsS -X POST "$WORKFLOW_A_HTTP/spot/action" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$WORKFLOW_SPOT_ID\",\"marker\":\"obs-a4\",\"value\":7}" \
    >"$LOG_DIR/workflow-action.json"
  sleep "$ROUTE_SETTLE_SECONDS"
  verify_scenario OBS-A4 publisherLog "$LOG_DIR/workflow-b-flow.log" \
    subscriberLogs "$LOG_DIR/workflow-a-flow.log;$LOG_DIR/workflow-b-flow.log" \
    timerLog "$LOG_DIR/play-a-flow.log"
  docker exec "$REDIS_CONTAINER" redis-cli CLIENT PAUSE 11000 ALL >/dev/null
  docker exec "$REDIS_CONTAINER" redis-cli PING >/dev/null
  for _ in $(seq 1 100); do
    curl_local -fsS "$WORKFLOW_B_HTTP/evidence" >"$LOG_DIR/workflow-b.fanout.evidence.json"
    curl_local -fsS "$WORKFLOW_A_HTTP/evidence" >"$LOG_DIR/workflow-a.fanout.evidence.json"
    if python3 - "$LOG_DIR/workflow-b.fanout.evidence.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
late = [m["value"] for m in body["metrics"]
        if m["name"] == "zlink.location.owner_lease.renew.lateness"]
raise SystemExit(0 if late and max(late) >= 0.5 else 1)
PY
    then break; fi
    sleep "$EVIDENCE_POLL_SECONDS"
  done
  verify_scenario OBS-B3 \
    beforePublisherEvidence "$LOG_DIR/workflow-b.fanout.before.json" \
    beforeSubscriberEvidence "$LOG_DIR/workflow-a.fanout.before.json" \
    publisherEvidence "$LOG_DIR/workflow-b.fanout.evidence.json" \
    subscriberEvidence "$LOG_DIR/workflow-a.fanout.evidence.json"
fi

if [[ "$SCENARIO" == "all" || "$SCENARIO" == "drain" ]]; then
  # Establish the play-a -> play-b route before the owner starts draining.
  # C1 verifies continuity of an existing route, not a first lookup after the
  # owner has already stopped accepting new placement and route discovery.
  curl_local -fsS -X POST "$PLAY_A_HTTP/spot/action" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$SPOT_ID\",\"marker\":\"before-drain\",\"value\":1}" \
    >"$LOG_DIR/action-before-drain.json"
  python3 - "$LOG_DIR/action-before-drain.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
assert body["marker"] == "before-drain", body
PY
  curl_local -fsS "$PLAY_B_HTTP/evidence" >"$LOG_DIR/play-b.before-drain.evidence.json"
  # OBS-C1 — draining excludes new placement while the typed row, lease, and
  # existing route traffic remain available throughout the propagation window.
  curl_local -fsS -X POST "$PLAY_B_HTTP/drain" -H 'Content-Type: application/json' -d '{"deadlineMs":10000}' >/dev/null
  for _ in $(seq 1 100); do
    curl_local -fsS "$PLAY_B_HTTP/evidence" >"$LOG_DIR/play-b.drain.evidence.json" || true
    if python3 - "$LOG_DIR/play-b.drain.evidence.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
states = [event["state"] for event in body["drainEvents"]]
ok = "draining" in states and body["ready"] is False
raise SystemExit(0 if ok else 1)
PY
    then break; fi
    sleep "$EVIDENCE_POLL_SECONDS"
  done
  python3 - "$LOG_DIR/play-b.drain.evidence.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
states = [event["state"] for event in body["drainEvents"]]
assert "draining" in states, states
assert body["ready"] is False, "play-b still ready after drain"
play_b_hex = "play-b".encode().hex()
play_b_rows = [row for row in body["peerRows"] if row["nodeRid"] == play_b_hex]
draining_rows = [row for row in play_b_rows if row["draining"]]
assert draining_rows, f"draining peer row was removed: {body['peerRows']}"
assert all(event["source"] == "drain" for event in body["drainEvents"]), body["drainEvents"]
metric_states = {m["tags"].get("state") for m in body["metrics"]
                if m["name"] == "zlink.drain.state"}
assert {"serving", "draining"} <= metric_states, metric_states
print("OBS-C1 marker PASS (row retained + readiness flip + drain metric transition)")
PY
  existing_route_successes=0
  for index in $(seq 1 8); do
    curl_local -fsS -X POST "$PLAY_A_HTTP/spot/action" -H 'Content-Type: application/json' \
      -d "{\"spotId\":\"$SPOT_ID\",\"marker\":\"drain-$index\",\"value\":1}" \
      >"$LOG_DIR/drain-action-$index.json"
    python3 - "$LOG_DIR/drain-action-$index.json" "$index" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
assert body["marker"] == f"drain-{sys.argv[2]}", body
PY
    existing_route_successes=$((existing_route_successes + 1))
    sleep 0.25
  done
  [[ "$existing_route_successes" -eq 8 ]] \
    || { echo "OBS-C1 existing route traffic did not complete 8/8" >&2; exit 1; }
  curl_local -fsS "$PLAY_B_HTTP/evidence" >"$LOG_DIR/play-b.drain-traffic.evidence.json"
  python3 - "$LOG_DIR/play-b.before-drain.evidence.json" \
    "$LOG_DIR/play-b.drain-traffic.evidence.json" <<'PY'
import json, sys
before = json.load(open(sys.argv[1], encoding="utf-8"))
during = json.load(open(sys.argv[2], encoding="utf-8"))
play_b_hex = "play-b".encode().hex()
rows = [row for row in during["peerRows"]
        if row["nodeRid"] == play_b_hex and row["draining"]]
assert rows, f"draining peer row disappeared during traffic: {during['peerRows']}"
before_leases = before.get("ownerLeases", [])
during_leases = during.get("ownerLeases", [])
assert len(before_leases) == 1 and len(during_leases) == 1, \
       f"owner lease evidence missing: before={before_leases}, during={during_leases}"
before_renewed = before_leases[0].get("renewedAtUnixMs")
during_renewed = during_leases[0].get("renewedAtUnixMs")
assert before_leases[0].get("healthy") is True, before_leases
assert during_leases[0].get("healthy") is True, during_leases
assert isinstance(before_renewed, int) and isinstance(during_renewed, int), \
       f"renewedAtUnixMs missing: before={before_renewed}, during={during_renewed}"
assert during_renewed > before_renewed, \
       f"owner lease did not renew while draining: before={before_renewed}, during={during_renewed}"
assert len(during["metrics"]) >= len(before["metrics"]), \
       "metric evidence regressed while existing traffic continued"
print("OBS-C1 existing route traffic PASS (8/8, descriptor retained, owner lease renewed)")
PY
  # The typed create result must explicitly report rejection; transport failure
  # cannot stand in for this application-visible result.
  curl_local -fsS -X POST "$PLAY_B_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d '{"spotId":"obs-room-rejected"}' >"$LOG_DIR/create-while-draining.json"
  python3 - "$LOG_DIR/create-while-draining.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
assert body.get("state") == "rejected", f"create while draining was not rejected: {body}"
PY
  echo "OBS-C1 create-rejection PASS"
  # Existing peer (play-a) stays ready and serving.
  curl_local -fsS "$PLAY_A_HTTP/evidence" >"$LOG_DIR/play-a.drain.evidence.json"
  python3 - "$LOG_DIR/play-a.drain.evidence.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
assert body["ready"] is True, "play-a lost readiness"
print("OBS-C1 peer-isolation PASS (play-a stays ready)")
PY
  # Terminal result within the deadline (no in-flight work left).
  for _ in $(seq 1 150); do
    curl_local -fsS "$PLAY_B_HTTP/evidence" >"$LOG_DIR/play-b.drain-final.evidence.json" || break
    if python3 - "$LOG_DIR/play-b.drain-final.evidence.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
states = [event["state"] for event in body["drainEvents"]]
raise SystemExit(0 if ("drained" in states or "force_stopping" in states) else 1)
PY
    then break; fi
    sleep "$EVIDENCE_POLL_SECONDS"
  done
  python3 - "$LOG_DIR/play-b.drain-final.evidence.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
states = [event["state"] for event in body["drainEvents"]]
assert "drained" in states, f"drain did not reach a terminal state: {states}"
print("OBS-C1 terminal PASS (drained)")
PY
  verify_scenario OBS-C1 duringEvidence "$LOG_DIR/play-b.drain.evidence.json" \
    finalEvidence "$LOG_DIR/play-b.drain-final.evidence.json" \
    rejectedCreate "$LOG_DIR/create-while-draining.json"
fi

relaunch_topology() {
  local phase="$1" trace_a="${2:-}" metrics_a="${3:-}"
  stop_roles
  assign_ports
  REDIS_KEY_PREFIX="zlink:cpp:observability-ops:${RUN_ID}:${phase}"
  PHASE_LOG_DIR="$LOG_DIR/$phase"
  launch_role "$SESSION_SERVER" session "$SESSION_HTTP" "$SESSION_ROUTE" "$PLAY_A_ROUTE" \
    "$SESSION_SPOT_ROUTER" "$SESSION_SPOT_PUB" "$STREAM_ENDPOINT" \
    "$PHASE_LOG_DIR" key_transitions on
  wait_health "$SESSION_HTTP"
  launch_role "$PLAY_SERVER" play-a "$PLAY_A_HTTP" "$PLAY_A_ROUTE" "$PLAY_B_ROUTE" \
    "$PLAY_A_SPOT_ROUTER" "$PLAY_A_SPOT_PUB" "" \
    "$PHASE_LOG_DIR" "${trace_a:-key_transitions}" "${metrics_a:-on}"
  wait_health "$PLAY_A_HTTP"
  launch_role "$PLAY_SERVER" play-b "$PLAY_B_HTTP" "$PLAY_B_ROUTE" "$PLAY_A_ROUTE" \
    "$PLAY_B_SPOT_ROUTER" "$PLAY_B_SPOT_PUB" "" \
    "$PHASE_LOG_DIR" key_transitions on
  wait_health "$PLAY_B_HTTP"
  launch_role "$ORDER_WORKFLOW_SERVER" workflow-a "$WORKFLOW_A_HTTP" "$WORKFLOW_A_ROUTE" \
    "$WORKFLOW_B_ROUTE" "$WORKFLOW_A_SPOT_ROUTER" "$WORKFLOW_A_SPOT_PUB" "" \
    "$PHASE_LOG_DIR"
  wait_health "$WORKFLOW_A_HTTP"
  launch_role "$ORDER_WORKFLOW_SERVER" workflow-b "$WORKFLOW_B_HTTP" "$WORKFLOW_B_ROUTE" \
    "$WORKFLOW_A_ROUTE" "$WORKFLOW_B_SPOT_ROUTER" "$WORKFLOW_B_SPOT_PUB" "" \
    "$PHASE_LOG_DIR"
  wait_health "$WORKFLOW_B_HTTP"
  sleep "$ROUTE_SETTLE_SECONDS"
}

drain_and_wait_terminal() {
  local http="$1" deadline_ms="$2" evidence_out="$3"
  curl_local -fsS -X POST "$http/drain" -H 'Content-Type: application/json' \
    -d "{\"deadlineMs\":$deadline_ms}" >/dev/null
  for _ in $(seq 1 200); do
    curl_local -fsS "$http/evidence" >"$evidence_out" 2>/dev/null || break
    if python3 - "$evidence_out" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
states = [event["state"] for event in body["drainEvents"]]
raise SystemExit(0 if ("drained" in states or "force_stopping" in states) else 1)
PY
    then break; fi
    sleep "$EVIDENCE_POLL_SECONDS"
  done
}

if [[ "$SCENARIO" == "all" || "$SCENARIO" == "handoff" || "$SCENARIO" == "metrics" ]]; then
  # OBS-C2 + OBS-C5(a) — drain moves the joined actor to the serving peer,
  # the transfer instruments fire, and the rolling drain ends Drained.
  relaunch_topology handoff
  curl_local -fsS -X POST "$PLAY_B_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d '{"spotId":"obs-b2-room"}' >"$PHASE_LOG_DIR/create-room.json"
  for index in $(seq 1 8); do
    curl_local -fsS -X POST "$PLAY_A_HTTP/spot/action" -H 'Content-Type: application/json' \
      -d "{\"spotId\":\"obs-b2-room\",\"marker\":\"obs-b2-$index\",\"value\":1}" \
      >"$PHASE_LOG_DIR/room-action-$index.json"
  done
  curl_local -fsS "$PLAY_B_HTTP/evidence" >"$PHASE_LOG_DIR/queue.evidence.json"
  curl_local -fsS -X POST "$PLAY_A_HTTP/actor/join" -H 'Content-Type: application/json' \
    -d '{"actorId":"obs-actor-1"}' >"$PHASE_LOG_DIR/join.json"
  python3 - "$PHASE_LOG_DIR/join.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
assert body["accepted"], body
PY
  curl_local -fsS -X POST "$PLAY_A_HTTP/actor/ping" -H 'Content-Type: application/json' \
    -d '{"actorId":"obs-actor-1","value":5}' >"$PHASE_LOG_DIR/ping-before.json"
  python3 - "$PHASE_LOG_DIR/ping-before.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
assert body["nodeRid"] == "play-a" and body["total"] == 5, body
PY
  # Warm the play-b -> play-a route (directory find + cross-node request)
  # before draining, so the handoff admission path has a live route mesh.
  ROUTE_WARM=1
  for _ in $(seq 1 100); do
    if curl_local -fsS -X POST "$PLAY_B_HTTP/actor/ping" -H 'Content-Type: application/json' \
        -d '{"actorId":"obs-actor-1","value":0}' >"$PHASE_LOG_DIR/ping-warm.json" 2>/dev/null; then
      ROUTE_WARM=0
      break
    fi
    sleep "$EVIDENCE_POLL_SECONDS"
  done
  ensure "$ROUTE_WARM" "handoff route warm-up (play-b -> play-a) never succeeded"
  drain_and_wait_terminal "$PLAY_A_HTTP" 20000 "$PHASE_LOG_DIR/play-a.evidence.json"
  python3 - "$PHASE_LOG_DIR/play-a.evidence.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
states = [event["state"] for event in body["drainEvents"]]
assert "drained" in states and "force_stopping" not in states, states
metrics = body["metrics"]
handed = [m for m in metrics if m["name"] == "zlink.drain.actors.handed_off"]
transfers = [m for m in metrics if m["name"] == "zlink.actor.transfers"]
duration = [m for m in metrics if m["name"] == "zlink.actor.transfer.duration"]
pending = [m for m in metrics if m["name"] == "zlink.actor.transfer.pending_requests.count"]
assert handed and sum(m["value"] for m in handed) >= 1, handed
assert transfers and sum(m["value"] for m in transfers) >= 1, transfers
assert duration and duration[0]["kind"] == "histogram" and duration[0]["unit"] == "s", duration
assert len(pending) == 1 and pending[0]["kind"] == "histogram", pending
print("OBS-C2 PASS (handoff completed with transfer instruments)")
print("OBS-C5a PASS (rolling drain ended Drained without force)")
PY
  curl_local -fsS -X POST "$PLAY_B_HTTP/actor/ping" -H 'Content-Type: application/json' \
    -d '{"actorId":"obs-actor-1","value":3}' >"$PHASE_LOG_DIR/ping-after.json"
  python3 - "$PHASE_LOG_DIR/ping-after.json" <<'PY'
import json, sys
body = json.load(open(sys.argv[1], encoding="utf-8"))
assert body["nodeRid"] == "play-b", body
print("OBS-C2 continuity PASS (post-move ping served by play-b)")
PY
  verify_scenario OBS-C2 sourceEvidence "$PHASE_LOG_DIR/play-a.evidence.json" \
    postMovePing "$PHASE_LOG_DIR/ping-after.json"
  verify_scenario OBS-B2 queueEvidence "$PHASE_LOG_DIR/queue.evidence.json" \
    transferEvidence "$PHASE_LOG_DIR/play-a.evidence.json"
fi

if [[ "$SCENARIO" == "all" || "$SCENARIO" == "force" ]]; then
  # OBS-C4 + OBS-C5(b) — no eligible target (peer already draining) plus a
  # short deadline forces the drain; the held session receives
  # session-closing(server_drain) and the connector exposes closeReason.
  relaunch_topology force
  curl_local -fsS -X POST "$PLAY_A_HTTP/actor/join" -H 'Content-Type: application/json' \
    -d '{"actorId":"obs-actor-2"}' >"$PHASE_LOG_DIR/join.json"
  # Establish the held session and its remote room route before draining the
  # only peer. This preserves the contract's ordering: a healthy bound session
  # exists first, then the topology loses every handoff target.
  curl_local -fsS -X POST "$PLAY_B_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d '{"spotId":"obs-room-force"}' >"$PHASE_LOG_DIR/create.json"
  sleep "$SCENARIO_SETTLE_SECONDS"
  write_trigger_config "$CONFIG_DIR/trigger-hold-session.json" hold-session "obs-room-force"
  "$CLIENT" --config="$CONFIG_DIR/trigger-hold-session.json" \
    >"$PHASE_LOG_DIR/hold-session.log" 2>&1 &
  HOLD_PID=$!
  PIDS+=("$HOLD_PID")
  for _ in $(seq 1 100); do
    if grep -q "hold-session ready" "$PHASE_LOG_DIR/hold-session.log" 2>/dev/null; then break; fi
    sleep "$LOCAL_READINESS_POLL_SECONDS"
  done
  grep -q "hold-session ready" "$PHASE_LOG_DIR/hold-session.log" || {
    echo "OBS-C4 failed: hold-session never became ready" >&2
    cat "$PHASE_LOG_DIR/hold-session.log" >&2
    exit 1
  }
  drain_and_wait_terminal "$PLAY_B_HTTP" 8000 "$PHASE_LOG_DIR/play-b.evidence.json"
  drain_and_wait_terminal "$PLAY_A_HTTP" 3000 "$PHASE_LOG_DIR/play-a.evidence.json"
  drain_and_wait_terminal "$SESSION_HTTP" 3000 "$PHASE_LOG_DIR/session.evidence.json"
  python3 - "$PHASE_LOG_DIR/play-a.evidence.json" \
    "$PHASE_LOG_DIR/session.evidence.json" <<'PY'
import json, sys
actor_body = json.load(open(sys.argv[1], encoding="utf-8"))
session_body = json.load(open(sys.argv[2], encoding="utf-8"))
session_states = [event["state"] for event in session_body["drainEvents"]]
assert "force_stopping" in session_states, session_states
forced_actor = [m for m in actor_body["metrics"] if m["name"] == "zlink.drain.forced"
                and m["tags"].get("kind") == "actor"]
forced_session = [m for m in session_body["metrics"] if m["name"] == "zlink.drain.forced"
                  and m["tags"].get("kind") == "session"]
assert forced_session and sum(m["value"] for m in forced_session) >= 1, forced_session
if forced_actor:
    assert sum(m["value"] for m in forced_actor) >= 1, forced_actor
    actor_outcome = "forced"
else:
    actor_outcome = "natural"
print(f"OBS-C5b PASS (actor={actor_outcome}, session=forced)")
PY
  wait "$HOLD_PID" || true
  grep -q "closeReason=server_drain" "$PHASE_LOG_DIR/hold-session.log" || {
    echo "OBS-C4 failed: connector did not observe server_drain" >&2
    cat "$PHASE_LOG_DIR/hold-session.log" >&2
    exit 1
  }
  echo "OBS-C4 PASS (forced stop notified the held session with server_drain)"
  verify_scenario OBS-C4 sessionEvidence "$PHASE_LOG_DIR/session.evidence.json" \
    connectorLog "$PHASE_LOG_DIR/hold-session.log"
  if [[ "$SCENARIO" == "all" ]]; then
    verify_scenario OBS-C5 rollingEvidence "$LOG_DIR/handoff/play-a.evidence.json" \
      forcedEvidence "$PHASE_LOG_DIR/session.evidence.json"
  fi
fi

if [[ "$SCENARIO" == "all" || "$SCENARIO" == "fixed-drain" ]]; then
  # OBS-C3 — one fixed lifecycle: a normal request leaves its Spot active,
  # drain rejects new turns, then closes local user Spots and owner rows.
  relaunch_topology fixed-drain
  FIXED_SPOT_ID="obs-c3-fixed-room"
  curl_local -fsS -X POST "$PLAY_B_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$FIXED_SPOT_ID\"}" >"$PHASE_LOG_DIR/create.json"
  curl_local -fsS -X POST "$PLAY_A_HTTP/spot/action" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$FIXED_SPOT_ID\",\"marker\":\"normal\",\"value\":1}" \
    >"$PHASE_LOG_DIR/normal-action.json"
  curl_local -fsS -X POST "$PLAY_B_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$FIXED_SPOT_ID\"}" >"$PHASE_LOG_DIR/still-open.json"
  python3 - "$PHASE_LOG_DIR/still-open.json" <<'PY'
import json, sys
assert json.load(open(sys.argv[1], encoding="utf-8"))["state"] == "existing"
PY
  curl_local -fsS -X POST "$PLAY_B_HTTP/drain" -H 'Content-Type: application/json' \
    -d '{"deadlineMs":15000}' >/dev/null
  curl_local -fsS -X POST "$PLAY_B_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"obs-c3-rejected\"}" >"$PHASE_LOG_DIR/rejected-create.json"
  drain_and_wait_terminal "$PLAY_B_HTTP" 15000 "$PHASE_LOG_DIR/drained.evidence.json"
  curl_local -fsS -X POST "$PLAY_B_HTTP/spot/close" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$FIXED_SPOT_ID\"}" >"$PHASE_LOG_DIR/close-after-drain.json"
  stale_status=$(curl_local -sS -o "$PHASE_LOG_DIR/stale-action-response.json" -w '%{http_code}' \
    -X POST "$PLAY_A_HTTP/spot/action" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$FIXED_SPOT_ID\",\"marker\":\"stale\",\"value\":1}" \
    || true)
  [[ "$stale_status" != "200" ]] || {
    echo "OBS-C3 stale handle unexpectedly resolved after owner cleanup" >&2
    exit 1
  }
  echo '{"accepted":false}' >"$PHASE_LOG_DIR/stale-action.json"
  curl_local -fsS -X POST "$PLAY_A_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$FIXED_SPOT_ID\"}" >"$PHASE_LOG_DIR/recreate.json"
  verify_scenario OBS-C3 normalAction "$PHASE_LOG_DIR/normal-action.json" \
    rejectedCreate "$PHASE_LOG_DIR/rejected-create.json" \
    drainedEvidence "$PHASE_LOG_DIR/drained.evidence.json" \
    closeAfterDrain "$PHASE_LOG_DIR/close-after-drain.json" \
    staleAction "$PHASE_LOG_DIR/stale-action.json" \
    recreate "$PHASE_LOG_DIR/recreate.json"
fi

if [[ "$SCENARIO" == "all" || "$SCENARIO" == "offnode" ]]; then
  # OBS-A3 — a tracing-off middle node creates nothing but the flow pair
  # still crosses it; OBS-B4 — no metric reader on play-a and messaging is
  # unchanged with an empty evidence collector.
  relaunch_topology offnode off off
  curl_local -fsS -X POST "$PLAY_B_HTTP/spot/create" -H 'Content-Type: application/json' \
    -d "{\"spotId\":\"$SPOT_ID\"}" >"$PHASE_LOG_DIR/create.json"
  sleep "$ROUTE_SETTLE_SECONDS"
  write_trigger_config "$CONFIG_DIR/trigger-offnode-flow.json" flow "$SPOT_ID"
  "$CLIENT" --config="$CONFIG_DIR/trigger-offnode-flow.json" \
    >"$PHASE_LOG_DIR/trigger-flow.log" 2>&1
  verify_scenario OBS-A3 upstreamLog "$PHASE_LOG_DIR/session-flow.log" \
    downstreamLog "$PHASE_LOG_DIR/play-b-flow.log" \
    offNodeLog "$PHASE_LOG_DIR/play-a-flow.log"
  curl_local -fsS "$PLAY_A_HTTP/evidence" >"$PHASE_LOG_DIR/play-a.evidence.json"
  verify_scenario OBS-B4 offNodeEvidence "$PHASE_LOG_DIR/play-a.evidence.json" \
    trafficEvidence "$PHASE_LOG_DIR/trigger-flow.log"
fi

cat <<'EOF'
DEFERRED (공개 계약의 남은 차이, see feature-map.ko.md):
  OBS-B1 connector reconnects 계기(공개 표면 spec 확정 대기 — 서버측 connections 계기는 검증됨),
  OBS-C2 bound-session push 연속성 세부(단언은 post-move ping 연속성으로 대체)
EOF
echo "ObservabilityOps scenario=$SCENARIO PASS"
