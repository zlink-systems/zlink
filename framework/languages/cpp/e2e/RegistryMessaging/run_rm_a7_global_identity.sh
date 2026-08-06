#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPP_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${ZLINK_CPP_E2E_BUILD_DIR:-$CPP_DIR/build}"
source "$SCRIPT_DIR/../redis-common.sh"

REDIS_CONTAINER=""
PIDS=()
REDIS_READINESS_TIMEOUT_SECONDS=30
cleanup () {
  local code=$?
  for pid in "${PIDS[@]:-}"; do
    kill "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
  done
  if [[ -n "$REDIS_CONTAINER" ]]; then
    zlink_redis_stop_scoped "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  fi
  exit "$code"
}
trap cleanup EXIT INT TERM

cmake -S "$CPP_DIR" -B "$BUILD_DIR" >/dev/null
cmake --build "$BUILD_DIR" --target zlink_cpp_e2e_registry_messaging_global_object_node >/dev/null

zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
  "zlink-redis-cpp-e2e-rm-a7" "redis:7-alpine"
REDIS_ENDPOINT="127.0.0.1:${redis_port}"
zlink_redis_wait_ready "$REDIS_CONTAINER" "$REDIS_READINESS_TIMEOUT_SECONDS"
KEY_PREFIX="zlink:cpp:e2e:rm-a7:$(date +%s)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$LOG_DIR"

read -r ROUTE_A ROUTE_B BRIDGE_A BRIDGE_B HTTP_A HTTP_B <<<"$(python3 - <<'PY'
import socket
values=[]
for _ in range(6):
    s=socket.socket(); s.bind(("127.0.0.1", 0)); values.append(s.getsockname()[1]); s.close()
print(*[f"tcp://127.0.0.1:{p}" for p in values[:4]], *[f"http://127.0.0.1:{p}" for p in values[4:]])
PY
)"

write_config () {
  local path="$1" rid="$2" mesh="$3" route="$4" bridge_route="$5" bridge_mesh="$6" bridge_peer="$7" http="$8" peer="$9"
  python3 - "$path" "$rid" "$mesh" "$route" "$bridge_route" "$bridge_mesh" "$bridge_peer" "$http" "$peer" "$REDIS_ENDPOINT" "$KEY_PREFIX" "$LOG_DIR" <<'PY'
import json, os, stat, sys
path, rid, mesh, route, bridge_route, bridge_mesh, bridge_peer, http, peer, redis, prefix, log_dir = sys.argv[1:]
with open(path, "w", encoding="utf-8") as f:
    json.dump({"e2e": {"rid": rid, "meshName": mesh, "routeEndpoint": route,
        "bridgeMeshName": bridge_mesh, "bridgeRouteEndpoint": bridge_route,
        "bridgePeerEndpoint": bridge_peer, "httpEndpoint": http, "peerEndpoints": peer, "redis": {"endpoint": redis,
        "keyPrefix": prefix}, "logDir": log_dir}}, f, indent=2)
os.chmod(path, stat.S_IRUSR | stat.S_IWUSR)
PY
}

write_config "$LOG_DIR/a.json" object-a profile-mesh "$ROUTE_A" "$BRIDGE_A" workflow-mesh "$ROUTE_B" "$HTTP_A" "$BRIDGE_B"
write_config "$LOG_DIR/b.json" object-b workflow-mesh "$ROUTE_B" "$BRIDGE_B" profile-mesh "$ROUTE_A" "$HTTP_B" "$BRIDGE_A"
BIN="$BUILD_DIR/zlink_cpp_e2e_registry_messaging_global_object_node"
PIDS=()
setsid "$BIN" --config="$LOG_DIR/a.json" >"$LOG_DIR/a.stdout.log" 2>"$LOG_DIR/a.stderr.log" & PIDS+=("$!")
setsid "$BIN" --config="$LOG_DIR/b.json" >"$LOG_DIR/b.stdout.log" 2>"$LOG_DIR/b.stderr.log" & PIDS+=("$!")

wait_health () {
  local url="$1"
  for _ in $(seq 1 60); do
    curl -fsS "$url/health" >/dev/null 2>&1 && return 0
    sleep 0.1
  done
  echo "RM-A7 health timeout: $url" >&2
  return 1
}
wait_health "$HTTP_A"
wait_health "$HTTP_B"
for _ in $(seq 1 60); do
  bridge_a=$(curl -fsS "$HTTP_A/status?mesh=workflow-mesh" 2>/dev/null || true)
  bridge_b=$(curl -fsS "$HTTP_B/status?mesh=profile-mesh" 2>/dev/null || true)
  main_a=$(curl -fsS "$HTTP_A/status?mesh=profile-mesh" 2>/dev/null || true)
  main_b=$(curl -fsS "$HTTP_B/status?mesh=workflow-mesh" 2>/dev/null || true)
  if [[ "$bridge_a" == *'"rid":"object-b","state":1'* \
        && "$bridge_b" == *'"rid":"object-a","state":1'* ]]; then
    break
  fi
  sleep 0.1
done
echo "RM-A7 bridge-a=$bridge_a bridge-b=$bridge_b main-a=$main_a main-b=$main_b"

post () {
  local url="$1" path="$2" body="$3" output="$4"
  curl -sS -o "$output" -w '%{http_code}' -H 'content-type: application/json' \
    -X POST "$url$path" --data "$body"
}
assert_same_ref () {
  local first="$1" second="$2"
  python3 - "$first" "$second" <<'PY'
import json, sys
a=json.load(open(sys.argv[1])); b=json.load(open(sys.argv[2]))
for key in ("nodeRid", "generation"):
    if a[key] != b[key]: raise SystemExit(f"RM-A7 reference mismatch: {key}")
PY
}

ACTOR_BODY='{"id":"rm-a7-global-actor","type":"global-actor"}'
SPOT_BODY='{"id":"rm-a7-global-spot","type":"global-user"}'
actor_a="$LOG_DIR/actor-a.json"; actor_b="$LOG_DIR/actor-b.json"
spot_a="$LOG_DIR/spot-a.json"; spot_b="$LOG_DIR/spot-b.json"
status_a="$LOG_DIR/status-a"; status_b="$LOG_DIR/status-b"
post "$HTTP_A" /object/create-actor "$ACTOR_BODY" "$actor_a" >"$status_a" & p1=$!
post "$HTTP_B" /object/create-actor "$ACTOR_BODY" "$actor_b" >"$status_b" & p2=$!
wait "$p1"; wait "$p2"
[[ "$(cat "$status_a")" == 200 && "$(cat "$status_b")" == 200 ]]
assert_same_ref "$actor_a" "$actor_b"

post "$HTTP_A" /object/create-spot "$SPOT_BODY" "$spot_a" >"$LOG_DIR/spot-status-a" & p1=$!
post "$HTTP_B" /object/create-spot "$SPOT_BODY" "$spot_b" >"$LOG_DIR/spot-status-b" & p2=$!
wait "$p1"; wait "$p2"
[[ "$(cat "$LOG_DIR/spot-status-a")" == 200 && "$(cat "$LOG_DIR/spot-status-b")" == 200 ]]
assert_same_ref "$spot_a" "$spot_b"

post "$HTTP_A" /object/find-actor '{"id":"rm-a7-global-actor"}' "$LOG_DIR/find-actor-a.json" >/dev/null
post "$HTTP_B" /object/find-actor '{"id":"rm-a7-global-actor"}' "$LOG_DIR/find-actor-b.json" >/dev/null
assert_same_ref "$LOG_DIR/find-actor-a.json" "$LOG_DIR/find-actor-b.json"
post "$HTTP_A" /object/find-spot '{"id":"rm-a7-global-spot"}' "$LOG_DIR/find-spot-a.json" >/dev/null
post "$HTTP_B" /object/find-spot '{"id":"rm-a7-global-spot"}' "$LOG_DIR/find-spot-b.json" >/dev/null
assert_same_ref "$LOG_DIR/find-spot-a.json" "$LOG_DIR/find-spot-b.json"

probe_body='{"kind":"actor","id":"rm-a7-global-actor"}'
post "$HTTP_A" /object/probe "$probe_body" "$LOG_DIR/probe-actor-a.json" >/dev/null
post "$HTTP_B" /object/probe "$probe_body" "$LOG_DIR/probe-actor-b.json" >/dev/null
probe_spot='{"kind":"spot","id":"rm-a7-global-spot"}'
post "$HTTP_A" /object/probe "$probe_spot" "$LOG_DIR/probe-spot-a.json" >/dev/null
post "$HTTP_B" /object/probe "$probe_spot" "$LOG_DIR/probe-spot-b.json" >/dev/null

python3 - "$actor_a" "$spot_a" "$LOG_DIR/probe-actor-a.json" "$LOG_DIR/probe-actor-b.json" "$LOG_DIR/probe-spot-a.json" "$LOG_DIR/probe-spot-b.json" <<'PY'
import json, sys
actor, spot = json.load(open(sys.argv[1])), json.load(open(sys.argv[2]))
for path, kind in zip(sys.argv[3:], ("actor", "actor", "spot", "spot")):
    value=json.load(open(path))
    if value["kind"] != kind or value["owner"] != (actor if kind == "actor" else spot)["nodeRid"]:
        raise SystemExit(f"RM-A7 direct probe mismatch: {path}")
PY

for url in "$HTTP_A" "$HTTP_B"; do
  status=$(post "$url" /object/create-actor '{"id":"rm-a7-global-actor","type":"other-actor"}' "$LOG_DIR/mismatch-actor.json")
  [[ "$status" == 409 ]]
  status=$(post "$url" /object/create-spot '{"id":"rm-a7-global-spot","type":"other-spot"}' "$LOG_DIR/mismatch-spot.json")
  [[ "$status" == 409 ]]
done

echo "registry-messaging RM-A7 result=passed log_dir=$LOG_DIR"
