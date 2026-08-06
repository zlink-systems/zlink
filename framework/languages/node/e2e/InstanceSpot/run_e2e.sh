#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
REPO_ROOT="$(git -C "$ROOT_DIR" rev-parse --show-toplevel)"
PACKAGE_ROOT="${ZLINK_NODE_FRAMEWORK_PACKAGE_ROOT:-$NODE_ROOT}"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$ROOT_DIR/log/$RUN_ID"
TEMP_DIR="$(mktemp -d)"
LOCAL_READINESS_ATTEMPTS=30
LOCAL_READINESS_POLL_SECONDS=0.1
LOCAL_READINESS_TIMEOUT_SECONDS=3
ROUTE_SETTLE_TIMEOUT_SECONDS=5
SCENARIO_SETTLE_TIMEOUT_SECONDS=3
HTTP_PROBE_TIMEOUT_SECONDS=3
mkdir -p "$LOG_DIR"

source "$NODE_ROOT/e2e/redis-container.sh"
source "$NODE_ROOT/e2e/runner-common.sh"

scenario="${1:-all}"
KNOWN_SCENARIOS=()
for number in $(seq 1 36); do
  KNOWN_SCENARIOS+=("IS-E2E-$(printf '%02d' "$number")")
done

contains() {
  local expected="$1"
  shift
  local value
  for value in "$@"; do
    [[ "$value" == "$expected" ]] && return 0
  done
  return 1
}

SELECTORS=()
if [[ "$scenario" == "all" || "$scenario" == "ALL" ]]; then
  SELECTORS=("${KNOWN_SCENARIOS[@]}")
else
  IFS=',' read -ra SELECTORS <<<"$scenario"
fi
for selector in "${SELECTORS[@]}"; do
  if ! contains "$selector" "${KNOWN_SCENARIOS[@]}"; then
    echo "Unknown InstanceSpot selector: $selector" >&2
    exit 2
  fi
done

if [[ ! -f "$PACKAGE_ROOT/node_modules/@zlink-systems/zlink/package.json" ]]; then
  echo "Node binding package is missing from package-mode root: $PACKAGE_ROOT" >&2
  exit 2
fi
if [[ ! -f "$PACKAGE_ROOT/packages/framework/dist/index.js" \
      || ! -f "$PACKAGE_ROOT/packages/nestjs/dist/index.js" ]]; then
  echo "Framework packages are not built in package-mode root: $PACKAGE_ROOT" >&2
  exit 2
fi

pids=()
REDIS_CONTAINER_ID=""
cleanup() {
  local code=$?
  stop_live_pids
  wait_all_pids_ignoring_status
  if [[ -n "$REDIS_CONTAINER_ID" ]]; then
    remove_redis_container
  fi
  rm -rf "$TEMP_DIR"
  if [[ "$code" -ne 0 ]]; then
    tail_failure_logs
  fi
}
trap cleanup EXIT

if [[ -n "${ZLINK_TEST_REDIS_ENDPOINT:-}" ]]; then
  REDIS_ENDPOINT="$ZLINK_TEST_REDIS_ENDPOINT"
elif command -v docker >/dev/null 2>&1; then
  start_redis_container "zlink-redis-instance-spot-${RANDOM}-$$" \
    -p "127.0.0.1::6379" "redis:7.2-alpine"
  REDIS_ENDPOINT="redis://$(redis_container_endpoint "$REDIS_CONTAINER_ID")"
else
  echo "InstanceSpot requires Docker Redis or ZLINK_TEST_REDIS_ENDPOINT." >&2
  exit 2
fi
wait_tcp redis "${REDIS_ENDPOINT#redis://}"

if [[ "$(realpath "$PACKAGE_ROOT")" != "$(realpath "$NODE_ROOT")" ]]; then
  mkdir -p "$PACKAGE_ROOT/e2e/InstanceSpot/Role" "$PACKAGE_ROOT/e2e/InstanceSpot/Client"
  cp "$ROOT_DIR/Role/main.ts" "$ROOT_DIR/Role/package.json" "$ROOT_DIR/Role/tsconfig.json" \
    "$PACKAGE_ROOT/e2e/InstanceSpot/Role/"
  cp "$ROOT_DIR/Client/main.ts" "$ROOT_DIR/Client/package.json" "$ROOT_DIR/Client/tsconfig.json" \
    "$PACKAGE_ROOT/e2e/InstanceSpot/Client/"
fi
build_package "$PACKAGE_ROOT/e2e/InstanceSpot/Role"
build_package "$PACKAGE_ROOT/e2e/InstanceSpot/Client"

OWNER_HTTP_PORT="$(allocate_port)"
CALLER_HTTP_PORT="$(allocate_port)"
OWNER_MESH_PORT="$(allocate_port)"
CALLER_MESH_PORT="$(allocate_port)"
OWNER_URL="http://127.0.0.1:$OWNER_HTTP_PORT"
CALLER_URL="http://127.0.0.1:$CALLER_HTTP_PORT"
OWNER_RID="instance-owner"
CALLER_RID="instance-caller"
REDIS_KEY_PREFIX="instance-spot:node:$RUN_ID"

python3 - "$TEMP_DIR" "$OWNER_HTTP_PORT" "$CALLER_HTTP_PORT" "$OWNER_MESH_PORT" \
  "$CALLER_MESH_PORT" "$REDIS_ENDPOINT" "$REDIS_KEY_PREFIX" <<'PY'
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
owner_http, caller_http, owner_mesh, caller_mesh = map(int, sys.argv[2:6])
redis_endpoint, redis_key_prefix = sys.argv[6:8]
values = {
    "owner": {
        "role": "owner", "rid": "instance-owner", "httpPort": owner_http,
        "meshEndpoint": f"tcp://127.0.0.1:{owner_mesh}",
        "redisEndpoint": redis_endpoint, "redisKeyPrefix": redis_key_prefix
    },
    "caller": {
        "role": "caller", "rid": "instance-caller", "httpPort": caller_http,
        "meshEndpoint": f"tcp://127.0.0.1:{caller_mesh}",
        "peerRid": "instance-owner",
        "peerEndpoint": f"tcp://127.0.0.1:{owner_mesh}",
        "redisEndpoint": redis_endpoint, "redisKeyPrefix": redis_key_prefix
    },
    "client": {
        "callerUrl": f"http://127.0.0.1:{caller_http}",
        "ownerUrl": f"http://127.0.0.1:{owner_http}",
        "ownerRid": "instance-owner"
    }
}
for name, value in values.items():
    (root / f"{name}.json").write_text(json.dumps(value), encoding="utf-8")
PY

ROLE_MAIN="$PACKAGE_ROOT/e2e/InstanceSpot/Role/dist/main.js"
CLIENT_MAIN="$PACKAGE_ROOT/e2e/InstanceSpot/Client/dist/main.js"
start_server owner "$ROLE_MAIN" --config="$TEMP_DIR/owner.json"
OWNER_PID="$LAST_STARTED_PID"
start_server caller "$ROLE_MAIN" --config="$TEMP_DIR/caller.json"
CALLER_PID="$LAST_STARTED_PID"
wait_health "$OWNER_URL" owner "$OWNER_PID"
wait_health "$CALLER_URL" caller "$CALLER_PID"

node "$CLIENT_MAIN" --config="$TEMP_DIR/client.json" "${SELECTORS[@]}" \
  >"$LOG_DIR/client.stdout.log" 2>"$LOG_DIR/client.stderr.log"
cat "$LOG_DIR/client.stdout.log"
echo "InstanceSpot PASS scenarios=${SELECTORS[*]} logs=$LOG_DIR"
