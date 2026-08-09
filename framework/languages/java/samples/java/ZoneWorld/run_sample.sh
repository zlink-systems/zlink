#!/usr/bin/env bash
set -euo pipefail
set +m

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)
cd "$ROOT_DIR"

if rg -n 'System\.(getProperty|getenv)|Thread\.sleep|sleep\(' Server Client --glob '*.java'; then
  echo "ZoneWorld application code must use config files and framework timers" >&2
  exit 1
fi

RUN_DIR="$(mktemp -d)"
chmod 0700 "$RUN_DIR"
LOG_DIR="$RUN_DIR/logs"
CONFIG_DIR="$RUN_DIR/config"
mkdir -p "$LOG_DIR" "$CONFIG_DIR"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="zone-a.log zone-b.log gateway.log ops.log"

pids=()
redis_container_id=""

print_logs() {
  local status="$1"
  [[ "$status" == "0" ]] && return
  for log in "$LOG_DIR"/*.log; do
    [[ -f "$log" ]] || continue
    echo "===== $log =====" >&2
    tail -n 240 "$log" >&2 || true
  done
}

cleanup_sample() {
  local status="$?"
  trap - EXIT
  set +e
  (exit "$status")
  cleanup
  local cleanup_status="$?"
  if [[ "${ZLINK_SAMPLE_KEEP_RUN_DIR:-0}" == "1" ]]; then
    echo "runDir=$RUN_DIR"
  else
    rm -rf "$RUN_DIR"
  fi
  if [[ "$status" != "0" ]]; then
    exit "$status"
  fi
  exit "$cleanup_status"
}
trap cleanup_sample EXIT

read -r mesh_a mesh_b mesh_gateway mesh_ops gateway_stream ops_stream <<<"$(zlink_sample_reserve_ports 6)"
zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-java-sample-zoneworld" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
redis_endpoint="127.0.0.1:${redis_port}"
redis_key_prefix="zoneworld:java:$(date +%s):$$:"

write_server_config() {
  local path="$1" role="$2" node_id="$3" mesh="$4" stream="$5"
  cat >"$path" <<EOF
sample.role=${role}
sample.node-id=${node_id}
sample.mesh-endpoint=tcp://127.0.0.1:${mesh}
sample.stream-endpoint=tcp://127.0.0.1:${stream}
sample.redis-endpoint=${redis_endpoint}
sample.redis-key-prefix=${redis_key_prefix}
EOF
  chmod 0600 "$path"
}

write_server_config "$CONFIG_DIR/zone-a.properties" zone zone-node-1 "$mesh_a" 0
write_server_config "$CONFIG_DIR/zone-b.properties" zone zone-node-2 "$mesh_b" 0
write_server_config "$CONFIG_DIR/gateway.properties" gateway gateway "$mesh_gateway" "$gateway_stream"
write_server_config "$CONFIG_DIR/ops.properties" ops ops "$mesh_ops" "$ops_stream"
cat >"$CONFIG_DIR/client.properties" <<EOF
sample.gateway-endpoint=tcp://127.0.0.1:${gateway_stream}
sample.ops-endpoint=tcp://127.0.0.1:${ops_stream}
sample.scenario=full
EOF
chmod 0600 "$CONFIG_DIR/client.properties"

(
  cd ../../..
  ./gradlew --no-daemon --no-parallel --max-workers=1 \
    :zlink-framework-core:jar \
    :zlink-framework-spring-boot-starter:jar \
    :zlink-framework-locations-redis:jar \
    :zlink-stream-connector:jar --quiet
)
gradle_run :Server:installDist :Client:installDist >/dev/null

start_role() {
  local name="$1"
  local config="$2"
  "$(app_bin Server Server)" --config "$config" >"$LOG_DIR/$name.log" 2>&1 &
  pids+=("$!")
}

start_role zone-a "$CONFIG_DIR/zone-a.properties"
wait_port "tcp://127.0.0.1:${mesh_a}"
deadline=$((SECONDS + 60))
while (( SECONDS < deadline )) && ! grep -q 'topology=ready node=zone-node-1' "$LOG_DIR/zone-a.log"; do sleep 0.1; done
grep -q 'topology=ready node=zone-node-1' "$LOG_DIR/zone-a.log"

start_role zone-b "$CONFIG_DIR/zone-b.properties"
start_role gateway "$CONFIG_DIR/gateway.properties"
start_role ops "$CONFIG_DIR/ops.properties"
wait_port "tcp://127.0.0.1:${mesh_b}"
wait_port "tcp://127.0.0.1:${mesh_gateway}"
wait_port "tcp://127.0.0.1:${mesh_ops}"
wait_port "tcp://127.0.0.1:${gateway_stream}"
wait_port "tcp://127.0.0.1:${ops_stream}"
wait_framework_ready_logs "$LOG_DIR" 1

"$(app_bin Client Client)" --config "$CONFIG_DIR/client.properties" >"$LOG_DIR/client.log" 2>&1
cat "$LOG_DIR/client.log"

grep -q 'zoneworld=completed' "$LOG_DIR/client.log"
grep -q 'zoneworld server evidence=completed' "$LOG_DIR/client.log"
grep -q 'topology=ready node=zone-node-1' "$LOG_DIR/zone-a.log"
grep -q 'topology=ready node=zone-node-2' "$LOG_DIR/zone-b.log"
grep -q 'runtime event mesh=zoneworld.mesh' "$LOG_DIR/zone-a.log"
grep -q 'fanout announcement delivered' "$LOG_DIR/zone-a.log"
grep -q 'fanout announcement delivered' "$LOG_DIR/zone-b.log"
grep -q 'report node=zone-node-1' "$LOG_DIR/ops.log"
grep -q 'report node=zone-node-2' "$LOG_DIR/ops.log"

echo "zoneworld full client/server self-check completed"
