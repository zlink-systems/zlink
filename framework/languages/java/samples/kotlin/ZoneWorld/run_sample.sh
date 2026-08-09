#!/usr/bin/env bash
set -euo pipefail
set +m

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)
cd "$ROOT_DIR"

if rg -n 'System\.(getProperty|getenv)|Thread\.sleep|sleep\(' Server Client --glob '*.kt'; then
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

read -r mesh_a mesh_b mesh_b_replacement mesh_gateway mesh_ops gateway_stream ops_stream \
  <<<"$(zlink_sample_reserve_ports 7)"
zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-kotlin-sample-zoneworld" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
redis_endpoint="127.0.0.1:${redis_port}"
redis_key_prefix="zoneworld:kotlin:$(date +%s):$$:"

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

write_client_config() {
  local path="$1" scenario="$2"
  cat >"$path" <<EOF
sample.gateway-endpoint=tcp://127.0.0.1:${gateway_stream}
sample.ops-endpoint=tcp://127.0.0.1:${ops_stream}
sample.scenario=${scenario}
EOF
  chmod 0600 "$path"
}

write_server_config "$CONFIG_DIR/zone-a.properties" zone zone-node-1 "$mesh_a" 0
write_server_config "$CONFIG_DIR/zone-b.properties" zone zone-node-2 "$mesh_b" 0
# The replacement is the same logical node on a different socket endpoint, in the same
# Store scope so that it takes over what the retired node owned. Reusing the retired
# endpoint would not exercise the replacement contract.
write_server_config "$CONFIG_DIR/zone-b-replacement.properties" zone zone-node-2 \
  "$mesh_b_replacement" 0
write_server_config "$CONFIG_DIR/gateway.properties" gateway gateway "$mesh_gateway" "$gateway_stream"
write_server_config "$CONFIG_DIR/ops.properties" ops ops "$mesh_ops" "$ops_stream"
write_client_config "$CONFIG_DIR/client.properties" full
write_client_config "$CONFIG_DIR/client-lifecycle.properties" lifecycle
write_client_config "$CONFIG_DIR/client-replacement.properties" replacement

(
  cd ../../..
  ./gradlew --no-daemon --no-parallel --max-workers=1 \
    :zlink-framework-core:jar \
    :zlink-framework-spring-boot-starter:jar \
    :zlink-framework-locations-redis:jar \
    :zlink-framework-kotlin:jar \
    :zlink-stream-connector:jar --quiet
)
gradle_run :Server:installDist :Client:installDist >/dev/null

role_pid=0
start_role() {
  local name="$1"
  local config="$2"
  "$(app_bin Server Server)" --config "$config" >"$LOG_DIR/$name.log" 2>&1 &
  role_pid="$!"
  pids+=("$role_pid")
}

# A pid this runner has already reaped must leave the cleanup list: waiting on it twice
# reports a failure for a process that stopped exactly as the scenario asked it to.
forget_pid() {
  local completed="$1" kept=() pid
  for pid in "${pids[@]+"${pids[@]}"}"; do
    [[ "$pid" == "$completed" ]] || kept+=("$pid")
  done
  pids=("${kept[@]+"${kept[@]}"}")
}

# ZW-C2, ZW-C3, ZW-E5 and ZW-G3 all need a node to go away. A graceful stop is what a
# deployment does, and it leaves the same complete lifecycle evidence every other role in
# this sample is held to.
stop_role() {
  local pid="$1"
  kill -TERM "$pid" >/dev/null 2>&1 || true
  wait "$pid" >/dev/null 2>&1 || true
  forget_pid "$pid"
}

log_line_count() {
  local file="$1"
  if [[ -f "$file" ]]; then wc -l <"$file"; else printf '0'; fi
}

wait_log() {
  local file="$1" pattern="$2" first_line="${3:-1}"
  local deadline=$((SECONDS + ${ZLINK_SAMPLE_LIFECYCLE_WAIT_SECONDS:-180}))
  while (( SECONDS < deadline )); do
    # grep -q can close its input on the first match, so the reader is a process
    # substitution: a SIGPIPE on tail must not turn a valid observation into a miss.
    if grep -q "$pattern" <(tail -n +"$first_line" "$file" 2>/dev/null); then
      return 0
    fi
    sleep 0.1
  done
  echo "Timed out waiting for '$pattern' in $file" >&2
  return 1
}

observed_routing_id() {
  local node="$1" first_line="${2:-1}"
  tail -n +"$first_line" "$LOG_DIR/ops.log" 2>/dev/null \
    | sed -nE "s/.*node status observed\. node=${node}, rid=([^,]+), registered=true.*/\1/p" \
    | tail -1
}

start_role zone-a "$CONFIG_DIR/zone-a.properties"
wait_port "tcp://127.0.0.1:${mesh_a}"
deadline=$((SECONDS + 60))
while (( SECONDS < deadline )) && ! grep -q 'topology=ready node=zone-node-1' "$LOG_DIR/zone-a.log"; do sleep 0.1; done
grep -q 'topology=ready node=zone-node-1' "$LOG_DIR/zone-a.log"

start_role zone-b "$CONFIG_DIR/zone-b.properties"
zone_b_pid="$role_pid"
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
grep -q 'scenario ZW-A2 passed' "$LOG_DIR/client.log"
grep -q 'scenario ZW-B7 passed' "$LOG_DIR/client.log"
grep -q 'topology=ready node=zone-node-1' "$LOG_DIR/zone-a.log"
grep -q 'topology=ready node=zone-node-2' "$LOG_DIR/zone-b.log"
grep -q 'runtime event mesh=zoneworld.mesh' "$LOG_DIR/zone-a.log"
grep -q 'fanout announcement delivered' "$LOG_DIR/zone-a.log"
grep -q 'fanout announcement delivered' "$LOG_DIR/zone-b.log"
grep -q 'report node=zone-node-1' "$LOG_DIR/ops.log"
grep -q 'report node=zone-node-2' "$LOG_DIR/ops.log"
# ZW-C2 and ZW-C3: the console has to watch the node go away, so the client arms itself on
# an established registration and the runner takes the node away underneath it. The same
# run closes the node first, which is the setup ZW-E5 restarts from.
"$(app_bin Client Client)" --config "$CONFIG_DIR/client-lifecycle.properties" \
  >"$LOG_DIR/client-lifecycle.log" 2>&1 &
lifecycle_pid="$!"
wait_log "$LOG_DIR/client-lifecycle.log" 'scenario ZW-E5 armed'
wait_log "$LOG_DIR/client-lifecycle.log" 'scenario ZW-C3 armed'
zone_b_routing_id="$(observed_routing_id zone-node-2)"
[[ -n "$zone_b_routing_id" ]] || {
  echo "Ops never observed a routing id for zone-node-2" >&2
  exit 1
}
stop_role "$zone_b_pid"
wait_log "$LOG_DIR/zone-b.log" 'ZLINK_FRAMEWORK_TERMINATION outcome=STOPPED reason=NONE'
wait "$lifecycle_pid"
cat "$LOG_DIR/client-lifecycle.log"
grep -q 'scenario ZW-C2 passed' "$LOG_DIR/client-lifecycle.log"
grep -q 'scenario ZW-C3 passed' "$LOG_DIR/client-lifecycle.log"

# ZW-G3: the same logical node comes back as a new process on a different endpoint.
ops_replacement_line=$(( $(log_line_count "$LOG_DIR/ops.log") + 1 ))
start_role zone-b2 "$CONFIG_DIR/zone-b-replacement.properties"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="$ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS zone-b2.log"
wait_port "tcp://127.0.0.1:${mesh_b_replacement}"
wait_log "$LOG_DIR/zone-b2.log" 'ZLINK_FRAMEWORK_READY'
wait_log "$LOG_DIR/zone-b2.log" 'topology=ready node=zone-node-2'
wait_log "$LOG_DIR/ops.log" \
  'node status observed\. node=zone-node-2, rid=.*, registered=true' "$ops_replacement_line"
replacement_routing_id="$(observed_routing_id zone-node-2 "$ops_replacement_line")"
[[ -n "$replacement_routing_id" ]] || {
  echo "Ops never observed a routing id for the zone-node-2 replacement" >&2
  exit 1
}
if [[ "$replacement_routing_id" == "$zone_b_routing_id" ]]; then
  echo "scenario ZW-G3 FAILED: the replacement reused the retired routing id" \
    "$zone_b_routing_id" >&2
  exit 1
fi
echo "scenario ZW-G3 identity passed old=$zone_b_routing_id new=$replacement_routing_id"

# ZW-E5: the node was closed before it stopped, and it reads that back from the store.
grep -q 'maintenance restored node=zone-node-2 enabled=true' "$LOG_DIR/zone-b2.log"

"$(app_bin Client Client)" --config "$CONFIG_DIR/client-replacement.properties" \
  >"$LOG_DIR/client-replacement.log" 2>&1
cat "$LOG_DIR/client-replacement.log"
grep -q 'scenario ZW-E5 passed' "$LOG_DIR/client-replacement.log"
grep -q 'scenario ZW-G3 ready' "$LOG_DIR/client-replacement.log"
# A replacement that is merely present proves nothing. One publish with no node list has to
# reach the new process and be accepted by the zone spots it built: a node that came up
# without the zones the retired one held cannot log this.
announcement_id="$(sed -nE 's/^scenario ZW-G3 announced id=(.+)$/\1/p' \
  "$LOG_DIR/client-replacement.log" | tail -1)"
[[ -n "$announcement_id" ]] || {
  echo "the replacement scenario never published an announcement" >&2
  exit 1
}
wait_log "$LOG_DIR/zone-b2.log" \
  "fanout announcement delivered node=zone-node-2 id=${announcement_id}"
echo "scenario ZW-G3 serves"
echo "scenario ZW-G3 passed"

echo "zoneworld full client/server self-check completed"
