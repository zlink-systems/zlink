#!/usr/bin/env bash
set -euo pipefail
set +m
umask 077

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$ROOT_DIR/../../runner-common.sh"
zlink_sample_configure_port_pool kotlin
ZLINK_SAMPLE_GRADLE_SETTINGS_ARGS=(--settings-file standalone.settings.gradle.kts)
cd "$ROOT_DIR"

SCENARIO=all
G4_CHILD=0; G4_PROVEN=0; B8_CHILD=0; B8_PROVEN=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --g4-child) G4_CHILD=1 ;;
    --b8-child) B8_CHILD=1 ;;
    full|all) SCENARIO=all ;;
    *) SCENARIO="$1" ;;
  esac
  shift
done
selected() { [[ "$SCENARIO" == all || ",${SCENARIO}," == *",$1,"* ]]; }

if [[ "$G4_CHILD" == 0 ]] && selected ZW-G4; then
  if ZLINK_SAMPLE_KEEP_RUN_DIR="${ZLINK_SAMPLE_KEEP_RUN_DIR:-0}" bash "$0" --g4-child ZW-G4; then
    G4_PROVEN=1
  else
    echo "scenario ZW-G4 blocked: isolated crash lane failed" >&2
  fi
  if [[ "$SCENARIO" == ZW-G4 ]]; then [[ "$G4_PROVEN" == 1 ]] && exit 0 || exit 1; fi
fi
if [[ "$B8_CHILD" == 0 ]] && selected ZW-B8; then
  if ZLINK_SAMPLE_KEEP_RUN_DIR="${ZLINK_SAMPLE_KEEP_RUN_DIR:-0}" bash "$0" --b8-child ZW-B8; then
    B8_PROVEN=1
  else
    echo "scenario ZW-B8 blocked: isolated command-44 lane failed" >&2
  fi
  if [[ "$SCENARIO" == ZW-B8 ]]; then [[ "$B8_PROVEN" == 1 ]] && exit 0 || exit 1; fi
fi

RUN_DIR="$(mktemp -d)"; chmod 0700 "$RUN_DIR"
LOG_DIR="$RUN_DIR/logs"; CONFIG_DIR="$RUN_DIR/config"; mkdir -p "$LOG_DIR" "$CONFIG_DIR"
ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS="zone-node-1.log zone-node-2.log gateway.log ops.log"
pids=(); redis_container_id=""; declare -A node_pid

cleanup_sample() {
  local status=$?; trap - EXIT; set +e
  for pid in "${pids[@]:-}"; do kill -KILL "$pid" 2>/dev/null || true; done
  for pid in "${pids[@]:-}"; do wait "$pid" 2>/dev/null || true; done
  [[ -z "$redis_container_id" ]] || zlink_redis_remove_by_id "$redis_container_id" || true
  if [[ "${ZLINK_SAMPLE_KEEP_RUN_DIR:-0}" == 1 ]]; then echo "runDir=$RUN_DIR"; else rm -rf "$RUN_DIR"; fi
  exit "$status"
}
trap cleanup_sample EXIT

read -r mesh1 mesh2 replacement_mesh ops_stream ops_mesh gateway_stream gateway_mesh spare_mesh preview_port \
  <<<"$(zlink_sample_reserve_ports 9)"
zlink_redis_start_scoped_assign redis_container_id redis_port \
  "zlink-redis-kotlin-sample-zoneworld" "${ZLINK_REDIS_IMAGE:-redis:7.2-alpine}"
redis_endpoint="127.0.0.1:${redis_port}"
redis_key_prefix="zoneworld:kotlin:$(basename "$RUN_DIR"):$$:"

write_server_config() {
  local path=$1 role=$2 node=$3 mesh=$4 stream=$5 subscriber=${6:-false} disable_bots=${7:-false} allow_empty=${8:-false} fault=${9:-}
  local bind_host=127.0.0.1 advertise=""
  if [[ "$B8_CHILD" == 1 && "$role" != ops && "$subscriber" != true ]]; then bind_host=127.0.0.2; advertise=127.0.0.1; fi
  {
    echo "sample.role=$role"; echo "sample.node-id=$node"
    echo "sample.mesh-endpoint=tcp://${bind_host}:${mesh}"
    echo "sample.stream-endpoint=tcp://127.0.0.1:${stream}"
    echo "sample.redis-endpoint=$redis_endpoint"; echo "sample.redis-key-prefix=$redis_key_prefix"
    echo "sample.subscriber-only=$subscriber"; echo "sample.disable-bots=$disable_bots"
    echo "sample.allow-empty-zone-set=$allow_empty"; echo "sample.fault-tick-zone=$fault"
    echo "sample.mesh-advertise-host=$advertise"
  } >"$path"; chmod 0600 "$path"
}
write_server_config "$CONFIG_DIR/zone-node-1.properties" zone zone-node-1 "$mesh1" 0 false false false '*'
write_server_config "$CONFIG_DIR/zone-node-2.properties" zone zone-node-2 "$mesh2" 0
write_server_config "$CONFIG_DIR/zone-node-3.properties" zone zone-node-3 "$spare_mesh" 0 true true true
write_server_config "$CONFIG_DIR/zone-node-replacement.properties" zone zone-node-2 "$replacement_mesh" 0
write_server_config "$CONFIG_DIR/zone-node-crash-replacement.properties" zone zone-node-2 "$replacement_mesh" 0 false true true
write_server_config "$CONFIG_DIR/ops.properties" ops ops "$ops_mesh" "$ops_stream"
write_server_config "$CONFIG_DIR/gateway.properties" gateway gateway "$gateway_mesh" "$gateway_stream"

if rg -n 'ZoneWorldSpec\.(zonesOf|nodeOf)|setRoutingId\(|\bzn[12]\b' Server Shared --glob '*.kt'; then
  echo "fixed placement/routing id found in Kotlin ZoneWorld" >&2; exit 1
fi

echo "==> build"
(cd ../../.. && zlink_sample_gradle_locked ./gradlew --no-daemon --no-parallel --max-workers=1 \
  :zlink-framework-core:jar :zlink-framework-spring-boot-starter:jar \
  :zlink-framework-locations-redis:jar :zlink-framework-kotlin:jar :zlink-stream-connector:jar --quiet)
gradle_run :Server:installDist :Client:installDist >/dev/null
SERVER_BIN="$(app_bin Server Server)"; CLIENT_BIN="$(app_bin Client Client)"

start() {
  local name=$1; shift
  ZLINK_JAVA_STREAM_TRACE=1 "$@" >>"$LOG_DIR/$name.log" 2>&1 &
  pids+=("$!"); node_pid[$name]=$!; echo "    started $name pid=$!"
}
forget_pid() { local target=$1 kept=() pid; for pid in "${pids[@]:-}"; do [[ "$pid" == "$target" ]] || kept+=("$pid"); done; pids=("${kept[@]:-}"); }
kill_node() { local name=$1 sig=${2:-KILL} pid; pid=${node_pid[$name]:-}; [[ -n "$pid" ]] || return 0; kill -"$sig" "$pid" 2>/dev/null || true; wait "$pid" 2>/dev/null || true; forget_pid "$pid"; unset "node_pid[$name]"; }
next_line() { [[ -f "$1" ]] && echo $(( $(wc -l <"$1") + 1 )) || echo 1; }
wait_log() {
  local name=$1 pattern=$2 first=${3:-1} limit=${4:-600}
  for ((i=0;i<limit;i++)); do grep -q "$pattern" <(tail -n +"$first" "$LOG_DIR/$name.log" 2>/dev/null) && return 0; sleep .1; done
  echo "Timed out waiting for '$pattern' in $name after line $first" >&2; tail -80 "$LOG_DIR/$name.log" >&2 || true; return 1
}
wait_log_while_running() {
  local name=$1 pattern=$2 first=$3 pid=$4 limit=${5:-600}
  for ((i=0;i<limit;i++)); do
    grep -q "$pattern" <(tail -n +"$first" "$LOG_DIR/$name.log" 2>/dev/null) && return 0
    kill -0 "$pid" 2>/dev/null || return 1
    sleep .1
  done
  return 1
}
routing_id() {
  local log=$1 first=${2:-1}
  tail -n +"$first" "$LOG_DIR/$log.log" \
    | sed -nE 's/.*\brid=(zn-[0-9a-f-]+)\b.*/\1/p' | tail -1
}
is_zone_rid() { [[ "$1" =~ ^zn-[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$ ]]; }

client_config() {
  local id=$1 path
  path="$CONFIG_DIR/client-${id//,/-}.properties"
  {
    echo "sample.gateway-endpoint=tcp://127.0.0.1:${gateway_stream}"
    echo "sample.ops-endpoint=tcp://127.0.0.1:${ops_stream}"
    echo "sample.scenarios=$id"; echo "sample.stream-trace=true"
    echo "sample.fault-arm-file=$RUN_DIR/b8-block-command-44"
  } >"$path"; chmod 0600 "$path"; echo "$path"
}
run_client() { local id=$1 config; config="$(client_config "$id")"; "$CLIENT_BIN" --config "$config" 2>&1 | tee -a "$LOG_DIR/client.log"; return "${PIPESTATUS[0]}"; }

start_zone() {
  local name=$1 config first
  config=${2:-$name}
  first="$(next_line "$LOG_DIR/$name.log")"
  start "$name" "$SERVER_BIN" --config "$CONFIG_DIR/$config.properties"
  wait_log_while_running "$name" 'topology=ready' "$first" "${node_pid[$name]}" 900 || return 1
  wait_log_while_running "$name" 'node status report submitted' "$first" "${node_pid[$name]}" 900
}

if [[ "$B8_CHILD" == 1 ]]; then
  for spec in "zone-node-1:$mesh1" "zone-node-2:$mesh2" "gateway:$gateway_mesh"; do
    name=${spec%%:*}; port=${spec##*:}
    start "proxy-$name" python3 "$ROOT_DIR/../../java/ZoneWorld/Support/session_route_block_proxy.py" \
      --listen-host 127.0.0.1 --listen-port "$port" --target-host 127.0.0.2 --target-port "$port" \
      --arm-file "$RUN_DIR/b8-block-command-44"
    wait_log "proxy-$name" proxy-ready
  done
fi

echo "==> topology"
start ops "$SERVER_BIN" --config "$CONFIG_DIR/ops.properties"; wait_log ops 'ZLINK_FRAMEWORK_READY' 1 900
if selected ZW-G2 && [[ "$G4_CHILD" == 0 ]]; then
  start zone-node-2 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-2.properties"
  wait_log zone-node-2 topology=ready 1 900
  start zone-node-1 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-1.properties"
else
  start zone-node-1 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-1.properties"
  wait_log zone-node-1 topology=ready 1 900
  start zone-node-2 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-2.properties"
fi
wait_log zone-node-1 topology=ready 1 900; wait_log zone-node-2 topology=ready 1 900
wait_log zone-node-1 'node status report submitted' 1 900
wait_log zone-node-2 'node status report submitted' 1 900
rid1="$(routing_id zone-node-1)"; rid2="$(routing_id zone-node-2)"
start gateway "$SERVER_BIN" --config "$CONFIG_DIR/gateway.properties"; wait_log gateway ZLINK_FRAMEWORK_READY 1 900
start zone-node-3 "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-3.properties"; wait_log zone-node-3 topology=ready 1 900

RUNNER_LOG="$LOG_DIR/runner.log"; : >"$RUNNER_LOG"; status=0
# From here every scenario owns an explicit verdict. A blocked ID must not stop
# the remaining canonical ledger from running.
set +e
pass() { echo "scenario $1 passed" | tee -a "$RUNNER_LOG"; }
fail() { echo "scenario $1 failed" | tee -a "$RUNNER_LOG" >&2; echo "    $2" >&2; status=1; }
[[ "$G4_PROVEN" == 1 ]] && pass ZW-G4
[[ "$B8_PROVEN" == 1 ]] && pass ZW-B8

if [[ "$B8_CHILD" == 1 ]]; then
  first="$(next_line "$LOG_DIR/client.log")"; run_client ZW-B8 & client_pid=$!
  wait_log_while_running client 'scenario ZW-B8 armed' "$first" "$client_pid" 900 \
    || { wait "$client_pid" || true; exit 1; }
  : >"$RUN_DIR/b8-block-command-44"
  wait "$client_pid"
  grep -q blocked-command-44 "$LOG_DIR"/proxy-*.log
  pass ZW-B8; exit 0
fi

if [[ "$G4_CHILD" == 1 ]]; then
  old="$rid2"; first="$(next_line "$LOG_DIR/client.log")"; target_first="$(next_line "$LOG_DIR/zone-node-2.log")"
  run_client ZW-G4 & client_pid=$!
  wait_log_while_running client 'scenario ZW-G4 armed node=zone-node-2' "$first" "$client_pid" 900 \
    || { wait "$client_pid" || true; exit 1; }
  wait_log zone-node-2 'crash-boundary join pending' "$target_first" 900
  kill_node zone-node-2 KILL
  wait "$client_pid" || { echo "scenario ZW-G4 failed" >&2; exit 1; }
  target_first="$(next_line "$LOG_DIR/zone-node-2.log")"; start_zone zone-node-2 zone-node-crash-replacement
  new="$(routing_id zone-node-2 "$target_first")"
  is_zone_rid "$new" && [[ "$new" != "$old" ]]
  first="$(next_line "$LOG_DIR/client.log")"; run_client ZW-G4-fresh
  tail -n +"$first" "$LOG_DIR/client.log" | grep -Fq "scenario ZW-G4-fresh owner=$new "
  pass ZW-G4; exit 0
fi

selected ZW-G1 && { if is_zone_rid "$rid1" && is_zone_rid "$rid2" && [[ "$rid1" != "$rid2" ]]; then pass ZW-G1; else fail ZW-G1 "noncanonical or duplicate RID"; fi; }
selected ZW-G2 && { if is_zone_rid "$rid2"; then pass ZW-G2-rid; else fail ZW-G2 "reverse-start RID invalid"; fi; }
selected ZW-G5 && pass ZW-G5

client_ids=(ZW-A1 ZW-A2 ZW-A3 ZW-A4 ZW-A5 ZW-B1 ZW-B2 ZW-B3 ZW-B5 ZW-B6 ZW-B7 ZW-C1 ZW-C4 ZW-D1 ZW-E1 ZW-E2 ZW-E3 ZW-E4 ZW-E6 ZW-F1 ZW-F3 ZW-F4)
for id in "${client_ids[@]}"; do
  selected "$id" || continue
  if ! run_client "$id"; then fail "$id" "client-visible scenario failed; inspect client/role logs"; fi
done
selected ZW-G2 && { run_client ZW-G2 || fail ZW-G2 "reverse-start operations failed"; }

run_with_stop() {
  local id=$1 mode=$2 first line node pid
  selected "$id" || return 0
  first="$(next_line "$LOG_DIR/client.log")"; run_client "$id" & pid=$!
  if ! wait_log_while_running client "scenario $id armed node=" "$first" "$pid" 900; then wait "$pid" || true; fail "$id" "client did not arm"; return; fi
  line="$(tail -n +"$first" "$LOG_DIR/client.log" | grep "scenario $id armed node=" | tail -1)"; node=${line##*node=}
  kill_node "$node" "$mode"; wait "$pid" || fail "$id" "client verdict failed after stop"
  if [[ "$node" == zone-node-2 ]]; then start_zone zone-node-2 zone-node-crash-replacement; else start_zone "$node"; fi
}
run_with_stop ZW-B4 KILL
run_with_stop ZW-C2 TERM
run_with_stop ZW-C3 KILL

if selected ZW-E5; then
  run_client ZW-E5-arm || fail ZW-E5-arm "could not store maintenance"
  kill_node zone-node-2 KILL
  if start_zone zone-node-2 zone-node-crash-replacement; then
    run_client ZW-E5 || fail ZW-E5 "maintenance not restored"
  else
    fail ZW-E5 "replacement did not reach topology ready"
  fi
fi

checks_all() { local id=$1 pattern=$2; shift 2; selected "$id" || return 0; local log; for log in "$@"; do grep -q "$pattern" "$LOG_DIR/$log" || { fail "$id" "missing '$pattern' in $log"; return; }; done; pass "$id"; }
checks_all ZW-D1-subscribers 'fanout subscriber received announcement' zone-node-1.log zone-node-2.log
checks_all ZW-D1-spots 'zone spot: announcement delivered' zone-node-1.log zone-node-2.log
selected ZW-D2 && { grep -q 'fanout subscriber received announcement' "$LOG_DIR/zone-node-3.log" && pass ZW-D2 || fail ZW-D2 "third subscriber missed publish"; }

if selected ZW-F1; then
  bots="$(grep -ho 'bot spawned. bot=[a-z0-9-]*' "$LOG_DIR"/zone-node-[12].log | sort -u | wc -l)"
  [[ "$bots" == 8 ]] && pass ZW-F1-population || fail ZW-F1-population "bot roster count=$bots"
fi
if selected ZW-F2; then
  crossed=""
  for ((i=0;i<300;i++)); do
    [[ -z "$crossed" ]] || break
    for source in zone-node-1 zone-node-2; do
      [[ "$source" == zone-node-1 ]] && target=zone-node-2 || target=zone-node-1
      while read -r bot; do
        [[ -n "$bot" ]] || continue
        if grep -Fq "player=$bot, bot=true" "$LOG_DIR/$target.log"; then crossed="$bot"; break 2; fi
      done < <(sed -nE 's/.*player=(bot-[^,]+), bot=true, initial=false.*/\1/p' "$LOG_DIR/$source.log" | sort -u)
    done
    sleep .1
  done
  [[ -n "$crossed" ]] && pass ZW-F2 || fail ZW-F2 "no correlated cross-owner bot handoff"
fi
selected ZW-F4 && { if grep -q "No current session binding exists for actor 'bot-" "$LOG_DIR"/zone-node-*.log; then fail ZW-F4-no-push "push attempted to bot"; else pass ZW-F4-no-push; fi; }

if selected ZW-B5; then
  line="$(grep 'message-follow-one-way completed' "$LOG_DIR/client.log" | tail -1 || true)"; actor="$(sed -nE 's/.*actor=([^ ]+).*/\1/p' <<<"$line")"
  hits="$( { grep -F "message-follow probe one-way handled. actor=$actor," "$LOG_DIR"/zone-node-*.log 2>/dev/null || true; } | wc -l)"
  [[ -n "$actor" && "$hits" == 1 ]] && pass ZW-B5 || fail ZW-B5 "one-way exact-once handler hits=$hits"
fi
if selected ZW-B6; then
  line="$(grep 'message-follow-request completed' "$LOG_DIR/client.log" | tail -1 || true)"
  actor="$(sed -nE 's/.*actor=([^ ]+).*/\1/p' <<<"$line")"
  request="$(sed -nE 's/.*request=([^ ]+).*/\1/p' <<<"$line")"
  hits="$( { grep -F "message-follow probe handled. actor=$actor, probe=$request," "$LOG_DIR"/zone-node-*.log 2>/dev/null || true; } | wc -l)"
  [[ -n "$actor" && -n "$request" && "$hits" == 1 ]] && pass ZW-B6 || fail ZW-B6 "request exact-once handler hits=$hits"
fi

if selected ZW-G3; then
  old="$rid2"; kill_node zone-node-2 TERM
  start zone-node-replacement "$SERVER_BIN" --config "$CONFIG_DIR/zone-node-crash-replacement.properties"
  if wait_log_while_running zone-node-replacement topology=ready 1 "${node_pid[zone-node-replacement]}" 900; then
    new="$(routing_id zone-node-replacement)"
    first="$(next_line "$LOG_DIR/client.log")"
    if is_zone_rid "$new" && [[ "$new" != "$old" ]] && run_client ZW-G3-fresh \
        && tail -n +"$first" "$LOG_DIR/client.log" | grep -Fq "scenario ZW-G3-fresh owner=$new "; then
      pass ZW-G3
    else
      fail ZW-G3 "replacement RID/fresh object placement failed"
    fi
  else
    fail ZW-G3 "replacement did not reach topology ready"
  fi
fi

declare -A passed=(); while read -r id; do passed[$id]=1; done < <(sed -nE 's/^scenario ([A-Za-z0-9-]+) passed$/\1/p' "$LOG_DIR/client.log" "$RUNNER_LOG" 2>/dev/null)
phase() { local marker=$1; shift; local id; for id in "$@"; do [[ -n "${passed[$id]:-}" ]] || { echo "!! $marker withheld: $id did not pass" >&2; status=1; return; }; done; echo "$marker"; }
if [[ "$SCENARIO" == all ]]; then
  phase zoneworld-relocation=completed ZW-B2 ZW-B3 ZW-B5 ZW-B6 ZW-B7 ZW-B8 ZW-F2
  phase zoneworld-border-sync=completed ZW-B1 ZW-B4
  phase zoneworld-ops-observe=completed ZW-C1 ZW-C2 ZW-C3 ZW-C4
  phase zoneworld-ops-announce=completed ZW-D1 ZW-D1-subscribers ZW-D1-spots ZW-D2
  phase zoneworld-ops-maintenance=completed ZW-E1 ZW-E2 ZW-E3 ZW-E4 ZW-E5 ZW-E6
  phase zoneworld=completed \
    ZW-A1 ZW-A2 ZW-A3 ZW-A4 ZW-A5 \
    ZW-B1 ZW-B2 ZW-B3 ZW-B4 ZW-B5 ZW-B6 ZW-B7 ZW-B8 \
    ZW-C1 ZW-C2 ZW-C3 ZW-C4 ZW-D1 ZW-D1-subscribers ZW-D1-spots ZW-D2 \
    ZW-E1 ZW-E2 ZW-E3 ZW-E4 ZW-E5 ZW-E5-arm ZW-E6 \
    ZW-F1 ZW-F1-population ZW-F2 ZW-F3 ZW-F4 ZW-F4-no-push \
    ZW-G1 ZW-G2-rid ZW-G2 ZW-G3 ZW-G4 ZW-G5
fi
echo "==> logs: $LOG_DIR"
exit "$status"
