#!/usr/bin/env bash
set -euo pipefail
set +m

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)
cd "$ROOT_DIR"
if rg -n -U '\.enableClient\(\s*[^)\s]|\.connect(?:Router|PeerPub)\(' Server --glob '*.java'; then
  echo "ShoppingMall server code must use location-store automatic connections" >&2
  exit 1
fi
RUN_DIR="$(mktemp -d)"
chmod 0700 "${RUN_DIR}"
LOG_DIR="$RUN_DIR/logs"
mkdir -p "$LOG_DIR"

read -r api_a_http_port api_b_http_port workflow_a_http_port workflow_b_http_port \
  workflow_a_channel_port workflow_b_channel_port workflow_a_spot_port workflow_b_spot_port \
  workflow_a_router_port workflow_b_router_port <<<"$(zlink_sample_reserve_ports 10)"
api_a_http="http://127.0.0.1:$api_a_http_port"
api_b_http="http://127.0.0.1:$api_b_http_port"
workflow_a_http="http://127.0.0.1:$workflow_a_http_port"
workflow_b_http="http://127.0.0.1:$workflow_b_http_port"
workflow_a_channel="tcp://127.0.0.1:$workflow_a_channel_port"
workflow_b_channel="tcp://127.0.0.1:$workflow_b_channel_port"
workflow_a_spot="tcp://127.0.0.1:$workflow_a_spot_port"
workflow_b_spot="tcp://127.0.0.1:$workflow_b_spot_port"
workflow_a_router="tcp://127.0.0.1:$workflow_a_router_port"
workflow_b_router="tcp://127.0.0.1:$workflow_b_router_port"

REDIS_CONTAINER=""
zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_PORT \
  "zlink-redis-java-sample-shoppingmall" "redis:7.2-alpine"
redis_endpoint="127.0.0.1:$REDIS_PORT"
redis_key_prefix="shoppingmall:java:$(date +%s):$$:"

pids=()
on_exit() {
  local status="$?"
  trap - EXIT
  if [[ "$status" != "0" ]]; then
    for log in "$LOG_DIR"/*.log; do
      [[ -f "$log" ]] || continue
      echo "===== $log =====" >&2
      tail -n 200 "$log" >&2 || true
    done
  fi
  cleanup
  rm -rf "$RUN_DIR"
  exit "$status"
}
trap on_exit EXIT

workflow_a_config="$RUN_DIR/workflow-a.properties"
workflow_b_config="$RUN_DIR/workflow-b.properties"
api_a_config="$RUN_DIR/api-a.properties"
api_b_config="$RUN_DIR/api-b.properties"
client_config="$RUN_DIR/client.properties"
write_common() {
  local path="$1"
  cat >>"$path" <<EOF
sample.logDirectory=${LOG_DIR}
sample.redisEndpoint=${redis_endpoint}
sample.redisKeyPrefix=${redis_key_prefix}
EOF
}
cat >"$workflow_a_config" <<EOF
sample.instanceName=workflow-a
sample.httpUrl=${workflow_a_http}
sample.channelEndpoint=${workflow_a_channel}
sample.spotEndpoint=${workflow_a_spot}
sample.spotRouterEndpoint=${workflow_a_router}
EOF
cat >"$workflow_b_config" <<EOF
sample.instanceName=workflow-b
sample.httpUrl=${workflow_b_http}
sample.channelEndpoint=${workflow_b_channel}
sample.spotEndpoint=${workflow_b_spot}
sample.spotRouterEndpoint=${workflow_b_router}
EOF
cat >"$api_a_config" <<EOF
sample.instanceName=api-a
sample.httpUrl=${api_a_http}
EOF
cat >"$api_b_config" <<EOF
sample.instanceName=api-b
sample.httpUrl=${api_b_http}
EOF
for config in "$workflow_a_config" "$workflow_b_config" "$api_a_config" "$api_b_config"; do write_common "$config"; done
cat >"$client_config" <<EOF
sample.apiAHttpUrl=${api_a_http}
sample.apiBHttpUrl=${api_b_http}
EOF
chmod 0600 "$workflow_a_config" "$workflow_b_config" "$api_a_config" "$api_b_config" "$client_config"

gradle_run :Server:CommerceApi:installDist :Server:OrderWorkflow:installDist :Client:installDist >"$LOG_DIR/build.log" 2>&1
start_role() {
  local name="$1" binary="$2" config="$3"
  "$binary" --config "$config" >"$LOG_DIR/$name.log" 2>&1 &
  pids+=("$!")
}
start_role workflow-a "$(app_bin Server/OrderWorkflow OrderWorkflow)" "$workflow_a_config"
start_role workflow-b "$(app_bin Server/OrderWorkflow OrderWorkflow)" "$workflow_b_config"
wait_port "$workflow_a_router"
wait_port "$workflow_b_router"
wait_http "$workflow_a_http"
wait_http "$workflow_b_http"

start_role api-a "$(app_bin Server/CommerceApi CommerceApi)" "$api_a_config"
start_role api-b "$(app_bin Server/CommerceApi CommerceApi)" "$api_b_config"
wait_http "$api_a_http"
wait_http "$api_b_http"
wait_framework_ready_logs "$LOG_DIR" 1
echo "topology=ready"
"$(app_bin Client Client)" --config "$client_config" >"$LOG_DIR/client.log" 2>&1
cat "$LOG_DIR/client.log"
grep -q "shoppingmall-server-evidence=completed" "$LOG_DIR/client.log"
grep -q "shoppingmall=completed" "$LOG_DIR/client.log"
grep -Rq "message flow" "$LOG_DIR"
echo "shoppingmall full client/server self-check completed"
