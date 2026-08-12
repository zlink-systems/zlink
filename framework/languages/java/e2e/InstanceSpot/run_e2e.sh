#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$(cd "${SCRIPT_DIR}/../.." && pwd)/e2e-redis-common.sh"
zlink_e2e_initialize java "$0" "$@"

SCENARIO="${1:-all}"
START_ORDER="forward"
if [[ "${2:-}" == "--start-order" ]]; then
  START_ORDER="${3:-forward}"
fi

cd "${SCRIPT_DIR}"

run_id="$(date +%Y%m%d-%H%M%S)-$$"
log_dir="${SCRIPT_DIR}/logs/${run_id}"
config_dir="$(mktemp -d)"
chmod 700 "${config_dir}"
repo_root="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"
default_core_lib="${repo_root}/core/build/lib/libzlink.so"
mkdir -p "${log_dir}"
status_file="${log_dir}/scenario-status.tsv"
printf 'scenario\tstatus\treason\n' >"${status_file}"

if [[ -z "${ZLINK_LIBRARY_PATH:-}" && -f "${default_core_lib}" ]]; then
  export ZLINK_LIBRARY_PATH="${default_core_lib}"
fi

e2e_build_dir="${HOME}/.cache/zlink/java-e2e/InstanceSpot"
gradle_cache_dir="${HOME}/.cache/zlink/java-e2e/InstanceSpot-gradle-cache"
redis_command_timeout_ms=500
location_heartbeat_ms=200
location_lease_ttl_ms=1500
location_polling_ms=100
location_store_failure_grace_ms=3000
location_key_prefix="zlink:e2e:instance-spot:${run_id}"
readiness_attempts=120
poll_interval_seconds=0.05

pids=()
REDIS_CONTAINER=""
last_pid=""
last_http_pid=""
topology_reason=""

OWNER_A_HTTP=""
OWNER_A_MESH=""
OWNER_A_PID=""
OWNER_B_HTTP=""
OWNER_B_MESH=""
OWNER_B_PID=""
CLIENT_A_HTTP=""
CLIENT_A_MESH=""
CLIENT_B_HTTP=""
CLIENT_B_MESH=""

scenario_ids=(
  IS-E2E-01 IS-E2E-02 IS-E2E-03 IS-E2E-04 IS-E2E-05 IS-E2E-06
  IS-E2E-07 IS-E2E-08 IS-E2E-09 IS-E2E-10 IS-E2E-11 IS-E2E-12
  IS-E2E-13 IS-E2E-14 IS-E2E-15 IS-E2E-16 IS-E2E-17 IS-E2E-18
  IS-E2E-19 IS-E2E-20 IS-E2E-21 IS-E2E-22 IS-E2E-23 IS-E2E-24
  IS-E2E-25 IS-E2E-26 IS-E2E-27 IS-E2E-28 IS-E2E-29 IS-E2E-30
  IS-E2E-31 IS-E2E-32 IS-E2E-33 IS-E2E-34 IS-E2E-35 IS-E2E-36
)

print_logs() {
  local status="$1"
  [[ "${status}" == "0" || "${status}" == "3" ]] && return
  for log in "${log_dir}"/*.stderr.log "${log_dir}"/scenario-*.log; do
    [[ -f "${log}" ]] || continue
    echo "===== ${log} =====" >&2
    tail -n 120 "${log}" >&2 || true
  done
}

descendants() {
  local pid="$1"
  local child
  (pgrep -P "${pid}" 2>/dev/null || true) | while read -r child; do
    descendants "${child}"
    echo "${child}"
  done
}

cleanup() {
  local status="$?"
  set +e
  print_logs "${status}"
  for ((index=${#pids[@]}-1; index>=0; index--)); do
    pid="${pids[${index}]}"
    for child in $(descendants "${pid}"); do
      kill "${child}" >/dev/null 2>&1 || true
    done
    kill "${pid}" >/dev/null 2>&1 || true
  done
  for _ in $(seq 1 50); do
    alive=0
    for pid in "${pids[@]}"; do
      if kill -0 "${pid}" >/dev/null 2>&1; then
        alive=1
        break
      fi
    done
    [[ "${alive}" == "0" ]] && break
    sleep 0.1
  done
  for ((index=${#pids[@]}-1; index>=0; index--)); do
    pid="${pids[${index}]}"
    for child in $(descendants "${pid}"); do
      kill -9 "${child}" >/dev/null 2>&1 || true
    done
    kill -9 "${pid}" >/dev/null 2>&1 || true
  done
  if [[ -n "${REDIS_CONTAINER}" ]]; then
    zlink_redis_remove_by_id "${REDIS_CONTAINER}" || true
  fi
  rm -rf "${config_dir}"
  wait >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT

reserve_ports() {
  local count="$1"
  zlink_e2e_reserve_ports "${count}"
}

http_endpoint() {
  echo "http://127.0.0.1:$1"
}

mesh_endpoint() {
  echo "tcp://127.0.0.1:$1"
}

port_of() {
  echo "${1##*:}"
}

gradle_run() {
  zlink_e2e_gradle_build_locked ../../gradlew \
    -PzlinkE2eBuildDir="${e2e_build_dir}" \
    --project-cache-dir "${gradle_cache_dir}" \
    --no-daemon \
    "$@" \
    --quiet
}

owner_bin() {
  echo "${e2e_build_dir}/Owner/install/instance-spot-owner/bin/instance-spot-owner"
}

client_bin() {
  echo "${e2e_build_dir}/Client/install/instance-spot-client/bin/instance-spot-client"
}

wait_http() {
  local name="$1"
  local endpoint="$2"
  local output="${log_dir}/${name}.health.json"
  for _ in $(seq 1 "${readiness_attempts}"); do
    if curl --max-time 1 --fail --silent --show-error \
        "${endpoint}/health" >"${output}" 2>"${log_dir}/${name}.health.stderr"; then
      return 0
    fi
    sleep "${poll_interval_seconds}"
  done
  echo "timed out waiting for ${name} at ${endpoint}" >&2
  return 1
}

write_owner_config() {
  local rid="$1"
  local lifecycle="$2"
  local http="$3"
  local mesh="$4"
  local evidence="$5"
  local path="${config_dir}/${rid}.properties"
  {
    echo "e2e.rid=${rid}"
    echo "e2e.lifecycle-id=${lifecycle}"
    echo "e2e.http-endpoint=${http}"
    echo "e2e.mesh-endpoint=${mesh}"
    echo "e2e.redis-location-endpoint=127.0.0.1:${redis_port}"
    echo "e2e.location-key-prefix=${location_key_prefix}"
    echo "e2e.redis-command-timeout-millis=${redis_command_timeout_ms}"
    echo "e2e.heartbeat-millis=${location_heartbeat_ms}"
    echo "e2e.lease-ttl-millis=${location_lease_ttl_ms}"
    echo "e2e.polling-millis=${location_polling_ms}"
    echo "e2e.store-failure-grace-millis=${location_store_failure_grace_ms}"
    echo "e2e.stable-type-limit=0"
    echo "e2e.disable-relocation=true"
    echo "e2e.evidence-file=${evidence}"
    echo "e2e.log-dir=${log_dir}"
  } >"${path}"
  chmod 600 "${path}"
  echo "${path}"
}

write_client_config() {
  local rid="$1"
  local http="$2"
  local mesh="$3"
  local path="${config_dir}/${rid}.properties"
  {
    echo "e2e.rid=${rid}"
    echo "e2e.http-endpoint=${http}"
    echo "e2e.mesh-endpoint=${mesh}"
    echo "e2e.redis-location-endpoint=127.0.0.1:${redis_port}"
    echo "e2e.location-key-prefix=${location_key_prefix}"
    echo "e2e.redis-command-timeout-millis=${redis_command_timeout_ms}"
    echo "e2e.heartbeat-millis=${location_heartbeat_ms}"
    echo "e2e.lease-ttl-millis=${location_lease_ttl_ms}"
    echo "e2e.polling-millis=${location_polling_ms}"
    echo "e2e.store-failure-grace-millis=${location_store_failure_grace_ms}"
    echo "e2e.log-dir=${log_dir}"
  } >"${path}"
  chmod 600 "${path}"
  echo "${path}"
}

start_owner() {
  local rid="$1"
  local lifecycle="$2"
  local http="$3"
  local mesh="$4"
  local evidence="$5"
  local config
  config="$(write_owner_config "${rid}" "${lifecycle}" "${http}" "${mesh}" "${evidence}")"
  "$(owner_bin)" --config "${config}" \
    >"${log_dir}/${rid}.stdout.log" \
    2>"${log_dir}/${rid}.stderr.log" &
  last_pid="$!"
  pids+=("${last_pid}")
  if [[ "${rid}" == "owner-a" ]]; then
    OWNER_A_PID="${last_pid}"
  else
    OWNER_B_PID="${last_pid}"
  fi
  wait_http "${rid}" "${http}"
}

start_client() {
  local rid="$1"
  local http="$2"
  local mesh="$3"
  local config
  config="$(write_client_config "${rid}" "${http}" "${mesh}")"
  "$(client_bin)" --config "${config}" \
    >"${log_dir}/${rid}.stdout.log" \
    2>"${log_dir}/${rid}.stderr.log" &
  last_pid="$!"
  pids+=("${last_pid}")
  wait_http "${rid}" "${http}"
}

setup_topology() {
  if ! command -v docker >/dev/null 2>&1; then
    topology_reason="Docker is required for the real Redis Location Store fixture"
    return 3
  fi

  if [[ "${ZLINK_E2E_REBUILD:-0}" == "1" \
      || ! -x "$(owner_bin)" || ! -x "$(client_bin)" ]]; then
    local -a build_args=(:Owner:installDist :Client:installDist)
    if [[ "${ZLINK_E2E_REBUILD:-0}" == "1" ]]; then
      build_args=(--refresh-dependencies clean "${build_args[@]}")
    fi
    if ! gradle_run "${build_args[@]}"; then
      topology_reason="Gradle installDist failed; see ${log_dir}/gradle output"
      return 1
    fi
  else
    echo "reusing existing fixture distributions owner=$(owner_bin) client=$(client_bin)"
  fi

  if ! zlink_redis_start_scoped_assign REDIS_CONTAINER redis_port \
      "zlink-redis-java-e2e-instance-spot" \
      "redis:7.2-alpine"; then
    topology_reason="Redis container could not be started"
    return 3
  fi

  read -r owner_a_http_port owner_a_mesh_port owner_b_http_port owner_b_mesh_port \
    client_a_http_port client_a_mesh_port client_b_http_port client_b_mesh_port \
    <<<"$(reserve_ports 8)"
  OWNER_A_HTTP="$(http_endpoint "${owner_a_http_port}")"
  OWNER_A_MESH="$(mesh_endpoint "${owner_a_mesh_port}")"
  OWNER_B_HTTP="$(http_endpoint "${owner_b_http_port}")"
  OWNER_B_MESH="$(mesh_endpoint "${owner_b_mesh_port}")"
  CLIENT_A_HTTP="$(http_endpoint "${client_a_http_port}")"
  CLIENT_A_MESH="$(mesh_endpoint "${client_a_mesh_port}")"
  CLIENT_B_HTTP="$(http_endpoint "${client_b_http_port}")"
  CLIENT_B_MESH="$(mesh_endpoint "${client_b_mesh_port}")"

  start_owner owner-a "${run_id}-owner-a" "${OWNER_A_HTTP}" "${OWNER_A_MESH}" \
    "${log_dir}/owner-a.evidence"
  start_owner owner-b "${run_id}-owner-b" "${OWNER_B_HTTP}" "${OWNER_B_MESH}" \
    "${log_dir}/owner-b.evidence"
  start_client client-a "${CLIENT_A_HTTP}" "${CLIENT_A_MESH}"
  start_client client-b "${CLIENT_B_HTTP}" "${CLIENT_B_MESH}"
}

get_json() {
  local endpoint="$1"
  local output="$2"
  curl --max-time 8 --fail --silent --show-error "${endpoint}" >"${output}"
}

post_json() {
  local endpoint="$1"
  local body="$2"
  local output="$3"
  curl --max-time 8 --fail --silent --show-error \
    -H 'Content-Type: application/json' \
    -X POST "${endpoint}" \
    --data "${body}" >"${output}"
}

post_json_async() {
  local endpoint="$1"
  local body="$2"
  local output="$3"
  local error_output="$4"
  curl --max-time 12 --fail --silent --show-error \
    -H 'Content-Type: application/json' \
    -X POST "${endpoint}" \
    --data "${body}" >"${output}" 2>"${error_output}" &
  last_http_pid="$!"
}

json_ready() {
  local path="$1"
  [[ -s "${path}" ]] || return 1
  python3 - "${path}" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    json.load(stream)
PY
}

wait_json() {
  local path="$1"
  local attempts="$2"
  for _ in $(seq 1 "${attempts}"); do
    if json_ready "${path}"; then
      return 0
    fi
    sleep "${poll_interval_seconds}"
  done
  return 1
}

wait_http_result() {
  local pid="$1"
  local output="$2"
  local attempts="$3"
  for _ in $(seq 1 "${attempts}"); do
    if json_ready "${output}"; then
      return 0
    fi
    if ! kill -0 "${pid}" >/dev/null 2>&1; then
      wait "${pid}" || true
      return 1
    fi
    sleep "${poll_interval_seconds}"
  done
  return 1
}

close_gate() {
  local gate_id="$1"
  local body="{\"gateId\":\"${gate_id}\",\"open\":false}"
  local endpoint output
  for endpoint in "${OWNER_A_HTTP}" "${OWNER_B_HTTP}"; do
    output="${log_dir}/gate-close-${gate_id}-$(port_of "${endpoint}").json"
    post_json "${endpoint}/gate" "${body}" "${output}"
  done
}

open_gate() {
  local gate_id="$1"
  local body="{\"gateId\":\"${gate_id}\",\"open\":true}"
  local endpoint output
  for endpoint in "${OWNER_A_HTTP}" "${OWNER_B_HTTP}"; do
    output="${log_dir}/gate-open-${gate_id}-$(port_of "${endpoint}").json"
    post_json "${endpoint}/gate" "${body}" "${output}"
  done
}

event_exists() {
  local path="$1"
  local kind="$2"
  local operation="$3"
  python3 - "${path}" "${kind}" "${operation}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    snapshot = json.load(stream)
kind = sys.argv[2]
operation = sys.argv[3]
print("1" if any(
    event["kind"] == kind and
    (not operation or event["operationId"] == operation)
    for event in snapshot["events"]
) else "0")
PY
}

wait_event_any() {
  local kind="$1"
  local operation="$2"
  local seconds="$3"
  local deadline=$((SECONDS + seconds))
  while (( SECONDS < deadline )); do
    for endpoint in "${OWNER_A_HTTP}" "${OWNER_B_HTTP}"; do
      path="${log_dir}/evidence-wait-$(port_of "${endpoint}")-${operation}.json"
      if curl --max-time 2 --fail --silent --show-error \
          "${endpoint}/evidence" >"${path}" 2>/dev/null \
          && [[ "$(event_exists "${path}" "${kind}" "${operation}")" == "1" ]]; then
        return 0
      fi
    done
    sleep "${poll_interval_seconds}"
  done
  return 1
}

snapshot_evidence() {
  local label="$1"
  get_json "${OWNER_A_HTTP}/evidence" "${log_dir}/${label}-owner-a.evidence.json"
  get_json "${OWNER_B_HTTP}/evidence" "${log_dir}/${label}-owner-b.evidence.json"
}

assert_request_success() {
  local path="$1"
  local spot_id="$2"
  local operation_id="$3"
  python3 - "${path}" "${spot_id}" "${operation_id}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    outcome = json.load(stream)
if not outcome.get("succeeded"):
    raise SystemExit(f"request failed: {outcome}")
reply = outcome.get("reply") or {}
if reply.get("spotId") != sys.argv[2] or reply.get("operationId") != sys.argv[3]:
    raise SystemExit(f"reply identity mismatch: {reply}")
if not reply.get("ownerRid") or reply.get("objectGeneration", 0) <= 0:
    raise SystemExit(f"reply lacks owner/generation evidence: {reply}")
PY
}

assert_lookup_ready() {
  local path="$1"
  local spot_id="$2"
  python3 - "${path}" "${spot_id}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    result = json.load(stream)
if not result.get("found") or result.get("spotId") != sys.argv[2]:
    raise SystemExit(f"public lookup is not Ready: {result}")
if result.get("objectGeneration", 0) <= 0 or not result.get("nodeRid"):
    raise SystemExit(f"public lookup lacks identity: {result}")
PY
}

assert_single_activation() {
  local spot_id="$1"
  local operations="$2"
  local path_a="$3"
  local path_b="$4"
  python3 - "${spot_id}" "${operations}" "${path_a}" "${path_b}" <<'PY'
import json
import sys

spot_id, operation_text, path_a, path_b = sys.argv[1:]
operations = operation_text.split(",") if operation_text else []
events = []
for path in (path_a, path_b):
    with open(path, encoding="utf-8") as stream:
        events.extend(json.load(stream)["events"])
factories = [event for event in events
             if event["kind"] == "FACTORY" and event["spotId"] == spot_id]
initializes = [event for event in events
               if event["kind"] == "INITIALIZE" and event["spotId"] == spot_id]
if len(factories) != 1 or len(initializes) != 1:
    raise SystemExit(
        f"activation count mismatch factories={len(factories)} "
        f"initializes={len(initializes)}")
owners = set()
for operation in operations:
    entered = [event for event in events
               if event["kind"] == "HANDLER_ENTER"
               and event["spotId"] == spot_id
               and event["operationId"] == operation]
    committed = [event for event in events
                 if event["kind"] == "HANDLER_COMMIT"
                 and event["spotId"] == spot_id
                 and event["operationId"] == operation]
    if len(entered) != 1 or len(committed) != 1:
        raise SystemExit(
            f"operation={operation} entered={len(entered)} "
            f"committed={len(committed)}")
    owners.add(entered[0]["ownerRid"])
    if entered[0]["activeHandlers"] > 1:
        raise SystemExit(f"active handler count exceeded one: {entered[0]}")
if len(owners) > 1:
    raise SystemExit(f"one Spot used multiple owners: {owners}")
PY
}

assert_reactivation() {
  local spot_id="$1"
  local first_operation="$2"
  local second_operation="$3"
  local path_a="$4"
  local path_b="$5"
  python3 - "${spot_id}" "${first_operation}" "${second_operation}" "${path_a}" "${path_b}" <<'PY'
import json
import sys

spot_id, first_operation, second_operation, path_a, path_b = sys.argv[1:]
events = []
for path in (path_a, path_b):
    with open(path, encoding="utf-8") as stream:
        events.extend(json.load(stream)["events"])
factories = [event for event in events
             if event["kind"] == "FACTORY" and event["spotId"] == spot_id]
initializes = [event for event in events
               if event["kind"] == "INITIALIZE" and event["spotId"] == spot_id]
if len(factories) != 2 or len(initializes) != 2:
    raise SystemExit(
        f"reactivation count mismatch factories={len(factories)} "
        f"initializes={len(initializes)}")
for operation in (first_operation, second_operation):
    commits = [event for event in events
               if event["kind"] == "HANDLER_COMMIT"
               and event["spotId"] == spot_id
               and event["operationId"] == operation]
    if len(commits) != 1:
        raise SystemExit(f"operation={operation} commit count={len(commits)}")
generations = {event["objectGeneration"] for event in factories}
if len(generations) != 2:
    raise SystemExit(f"reactivation reused generation: {generations}")
close_events = [event for event in events
                if event["kind"] == "CLOSE_RESULT" and event["spotId"] == spot_id]
if len(close_events) != 1:
    raise SystemExit(f"close evidence count={len(close_events)}")
PY
}

assert_concurrent() {
  local path_a="$1"
  local path_b="$2"
  local spot_id="$3"
  python3 - "${path_a}" "${path_b}" "${spot_id}" <<'PY'
import json
import sys

paths = sys.argv[1:3]
spot_id = sys.argv[3]
operations = []
for path in paths:
    with open(path, encoding="utf-8") as stream:
        result = json.load(stream)
    for outcome in result["outcomes"]:
        if not outcome.get("succeeded"):
            raise SystemExit(f"concurrent request failed: {outcome}")
        reply = outcome.get("reply") or {}
        if reply.get("spotId") != spot_id:
            raise SystemExit(f"wrong Spot in reply: {reply}")
        operations.append(reply["operationId"])
if len(operations) != len(set(operations)):
    raise SystemExit(f"duplicate operation reply: {operations}")
PY
}

public_probe() {
  local id="$1"
  local probe_id="probe-${id}-${run_id}"
  local lookup_path="${log_dir}/${id}.lookup.json"
  local health_path="${log_dir}/${id}.client-health.json"
  get_json "${CLIENT_A_HTTP}/health" "${health_path}"
  post_json "${CLIENT_A_HTTP}/lookup" \
    "{\"spotId\":\"${probe_id}\"}" "${lookup_path}"
  snapshot_evidence "${id}"
  echo "public_process_evidence=${health_path},${lookup_path},${log_dir}/${id}-owner-a.evidence.json,${log_dir}/${id}-owner-b.evidence.json"
}

blocked_scenario() {
  local id="$1"
  local reason="$2"
  public_probe "${id}"
  echo "BLOCKED ${id}: ${reason}"
  echo "reason=${reason}" >"${log_dir}/${id}.blocked.txt"
  return 3
}

scenario_01() {
  local spot_id="cold-request-${run_id}"
  local operation_id="op-01-${run_id}"
  local request_path="${log_dir}/IS-E2E-01.request.json"
  local lookup_path="${log_dir}/IS-E2E-01.lookup.json"
  post_json "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${operation_id}\",\"payload\":\"cold-request\",\"timeoutMilliseconds\":5000}" \
    "${request_path}"
  assert_request_success "${request_path}" "${spot_id}" "${operation_id}"
  post_json "${CLIENT_A_HTTP}/lookup" "{\"spotId\":\"${spot_id}\"}" "${lookup_path}"
  assert_lookup_ready "${lookup_path}" "${spot_id}"
  snapshot_evidence "IS-E2E-01"
  assert_single_activation "${spot_id}" "${operation_id}" \
    "${log_dir}/IS-E2E-01-owner-a.evidence.json" \
    "${log_dir}/IS-E2E-01-owner-b.evidence.json"
  echo "PASS IS-E2E-01 process=owner-a/owner-b client=client-a wire=typed-request+reply lookup=public"
}

scenario_02() {
  local spot_id="cold-send-${run_id}"
  local operation_id="op-02-${run_id}"
  local gate_id="gate-02-${run_id}"
  local send_path="${log_dir}/IS-E2E-02.send.json"
  close_gate "${gate_id}"
  post_json_async "${CLIENT_A_HTTP}/send" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${operation_id}\",\"payload\":\"gate:${gate_id}\"}" \
    "${send_path}" "${log_dir}/IS-E2E-02.send.stderr"
  if wait_http_result "${last_http_pid}" "${send_path}" 20; then
    python3 - "${send_path}" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    result = json.load(stream)
if not result.get("succeeded"):
    raise SystemExit(f"cold send failed: {result}")
PY
    snapshot_evidence "IS-E2E-02-before-open"
    if rg -n "\|SEND_HANDLER\|.*\|${operation_id}\|" \
        "${log_dir}/owner-a.evidence" "${log_dir}/owner-b.evidence" >/dev/null 2>&1; then
      open_gate "${gate_id}"
      echo "BLOCKED IS-E2E-02: send did not return before the gated handler evidence; public send completion is coupled to handler completion in this runtime"
      return 3
    fi
    open_gate "${gate_id}"
    echo "PASS IS-E2E-02 process=owner-a/owner-b client=client-a wire=one-way-submit-before-handler"
    return 0
  fi
  open_gate "${gate_id}"
  wait "${last_http_pid}" >/dev/null 2>&1 || true
  echo "BLOCKED IS-E2E-02: the gated cold send did not complete before handler release; see ${send_path} and ${log_dir}/IS-E2E-02-before-open-owner-*.evidence.json"
  return 3
}

scenario_03() {
  local spot_id="concurrent-first-${run_id}"
  local path_a="${log_dir}/IS-E2E-03.client-a.json"
  local path_b="${log_dir}/IS-E2E-03.client-b.json"
  local body_a="{\"spotId\":\"${spot_id}\",\"count\":8,\"operationPrefix\":\"op-03-a-${run_id}\",\"timeoutMilliseconds\":10000}"
  local body_b="{\"spotId\":\"${spot_id}\",\"count\":8,\"operationPrefix\":\"op-03-b-${run_id}\",\"timeoutMilliseconds\":10000}"
  post_json_async "${CLIENT_A_HTTP}/concurrent" "${body_a}" "${path_a}" "${log_dir}/IS-E2E-03.client-a.stderr"
  local pid_a="${last_http_pid}"
  post_json_async "${CLIENT_B_HTTP}/concurrent" "${body_b}" "${path_b}" "${log_dir}/IS-E2E-03.client-b.stderr"
  local pid_b="${last_http_pid}"
  wait_http_result "${pid_a}" "${path_a}" 240 || { wait "${pid_a}" || true; return 1; }
  wait_http_result "${pid_b}" "${path_b}" 240 || { wait "${pid_b}" || true; return 1; }
  assert_concurrent "${path_a}" "${path_b}" "${spot_id}"
  python3 - "${path_a}" "${path_b}" <<'PY'
import json
import sys
operations = []
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as stream:
        operations.extend(
            reply["reply"]["operationId"]
            for reply in json.load(stream)["outcomes"])
print(",".join(operations))
PY
  operations="$(python3 - "${path_a}" "${path_b}" <<'PY'
import json
import sys
operations = []
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as stream:
        operations.extend(
            reply["reply"]["operationId"]
            for reply in json.load(stream)["outcomes"])
print(",".join(operations))
PY
)"
  snapshot_evidence "IS-E2E-03"
  assert_single_activation "${spot_id}" "${operations}" \
    "${log_dir}/IS-E2E-03-owner-a.evidence.json" \
    "${log_dir}/IS-E2E-03-owner-b.evidence.json"
  echo "PASS IS-E2E-03 process=owner-a/owner-b client=client-a+client-b wire=16-typed-request+reply"
}

scenario_04() {
  local spot_a="different-a-${run_id}"
  local spot_b="different-b-${run_id}"
  local operation_a="op-04-a-${run_id}"
  local operation_b="op-04-b-${run_id}"
  local gate_id="gate-04-${run_id}"
  local path_a="${log_dir}/IS-E2E-04.a.json"
  local path_b="${log_dir}/IS-E2E-04.b.json"
  close_gate "${gate_id}"
  post_json_async "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot_a}\",\"operationId\":\"${operation_a}\",\"payload\":\"gate:${gate_id}\",\"timeoutMilliseconds\":10000}" \
    "${path_a}" "${log_dir}/IS-E2E-04.a.stderr"
  local pid_a="${last_http_pid}"
  wait_event_any HANDLER_ENTER "${operation_a}" 8
  post_json_async "${CLIENT_B_HTTP}/request" \
    "{\"spotId\":\"${spot_b}\",\"operationId\":\"${operation_b}\",\"payload\":\"different-b\",\"timeoutMilliseconds\":5000}" \
    "${path_b}" "${log_dir}/IS-E2E-04.b.stderr"
  local pid_b="${last_http_pid}"
  if ! wait_http_result "${pid_b}" "${path_b}" 60; then
    open_gate "${gate_id}"
    wait "${pid_a}" >/dev/null 2>&1 || true
    wait "${pid_b}" >/dev/null 2>&1 || true
    echo "Spot B did not complete while Spot A was gated" >&2
    return 1
  fi
  assert_request_success "${path_b}" "${spot_b}" "${operation_b}"
  open_gate "${gate_id}"
  wait "${pid_a}" >/dev/null 2>&1 || true
  assert_request_success "${path_a}" "${spot_a}" "${operation_a}"
  snapshot_evidence "IS-E2E-04"
  assert_single_activation "${spot_a}" "${operation_a}" \
    "${log_dir}/IS-E2E-04-owner-a.evidence.json" \
    "${log_dir}/IS-E2E-04-owner-b.evidence.json"
  assert_single_activation "${spot_b}" "${operation_b}" \
    "${log_dir}/IS-E2E-04-owner-a.evidence.json" \
    "${log_dir}/IS-E2E-04-owner-b.evidence.json"
  echo "PASS IS-E2E-04 process=owner-a/owner-b client=client-a+client-b wire=independent-spot-replies"
}

scenario_08() {
  local spot_id="close-reactivate-${run_id}"
  local first_operation="op-08-first-${run_id}"
  local close_operation="op-08-close-${run_id}"
  local second_operation="op-08-second-${run_id}"
  local first_path="${log_dir}/IS-E2E-08.first.json"
  local close_path="${log_dir}/IS-E2E-08.close.json"
  local second_path="${log_dir}/IS-E2E-08.second.json"
  post_json "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${first_operation}\",\"payload\":\"before-close\",\"timeoutMilliseconds\":5000}" \
    "${first_path}"
  assert_request_success "${first_path}" "${spot_id}" "${first_operation}"
  first_generation="$(python3 - "${first_path}" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(json.load(stream)["reply"]["objectGeneration"])
PY
)"
  post_json "${CLIENT_A_HTTP}/close" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${close_operation}\",\"gateId\":\"\"}" \
    "${close_path}"
  wait_event_any CLOSE_RESULT "${close_operation}" 15
  post_json "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${second_operation}\",\"payload\":\"after-close\",\"timeoutMilliseconds\":5000}" \
    "${second_path}"
  assert_request_success "${second_path}" "${spot_id}" "${second_operation}"
  second_generation="$(python3 - "${second_path}" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(json.load(stream)["reply"]["objectGeneration"])
PY
)"
  [[ "${first_generation}" != "${second_generation}" ]] || {
    echo "Close reused generation ${first_generation}" >&2
    return 1
  }
  snapshot_evidence "IS-E2E-08"
  assert_reactivation "${spot_id}" "${first_operation}" "${second_operation}" \
    "${log_dir}/IS-E2E-08-owner-a.evidence.json" \
    "${log_dir}/IS-E2E-08-owner-b.evidence.json"
  echo "PASS IS-E2E-08 process=owner-a/owner-b client=client-a wire=close-packet+new-generation-reply"
}

scenario_19() {
  local spot_id="ready-ordering-${run_id}"
  local first_operation="op-19-first-${run_id}"
  local follow_operation="op-19-follow-${run_id}"
  local gate_id="gate-19-${run_id}"
  local first_path="${log_dir}/IS-E2E-19.first.json"
  local follow_path="${log_dir}/IS-E2E-19.follow.json"
  close_gate "${gate_id}"
  post_json_async "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${first_operation}\",\"payload\":\"gate:${gate_id}\",\"timeoutMilliseconds\":10000}" \
    "${first_path}" "${log_dir}/IS-E2E-19.first.stderr"
  local pid_first="${last_http_pid}"
  wait_event_any HANDLER_ENTER "${first_operation}" 8
  post_json_async "${CLIENT_B_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${follow_operation}\",\"payload\":\"follow-up\",\"timeoutMilliseconds\":10000}" \
    "${follow_path}" "${log_dir}/IS-E2E-19.follow.stderr"
  local pid_follow="${last_http_pid}"
  if wait_http_result "${pid_follow}" "${follow_path}" 20; then
    open_gate "${gate_id}"
    wait "${pid_first}" >/dev/null 2>&1 || true
    echo "follow-up completed while first handler gate was closed" >&2
    return 1
  fi
  open_gate "${gate_id}"
  wait "${pid_first}" >/dev/null 2>&1 || true
  wait "${pid_follow}" >/dev/null 2>&1 || true
  assert_request_success "${first_path}" "${spot_id}" "${first_operation}"
  assert_request_success "${follow_path}" "${spot_id}" "${follow_operation}"
  snapshot_evidence "IS-E2E-19"
  python3 "${log_dir}/IS-E2E-19-owner-a.evidence.json" \
    "${log_dir}/IS-E2E-19-owner-b.evidence.json" "${spot_id}" \
    "${first_operation}" "${follow_operation}" <<'PY'
import json
import sys

paths = sys.argv[1:3]
spot, first, follow = sys.argv[3:]
events = []
for path in paths:
    with open(path, encoding="utf-8") as stream:
        events.extend(json.load(stream)["events"])
ordered = [event["operationId"] for event in sorted(events, key=lambda event: event["sequence"])
           if event["kind"] == "HANDLER_ENTER" and event["spotId"] == spot]
if ordered[:2] != [first, follow]:
    raise SystemExit(f"ready ordering mismatch: {ordered}")
PY
  echo "PASS IS-E2E-19 process=owner-a/owner-b client=client-a+client-b wire=first-request-before-follow-up"
}

scenario_26() {
  local spot_id="concurrent-claim-${run_id}"
  local path_a="${log_dir}/IS-E2E-26.client-a.json"
  local path_b="${log_dir}/IS-E2E-26.client-b.json"
  post_json_async "${CLIENT_A_HTTP}/concurrent" \
    "{\"spotId\":\"${spot_id}\",\"count\":6,\"operationPrefix\":\"op-26-a-${run_id}\",\"timeoutMilliseconds\":10000}" \
    "${path_a}" "${log_dir}/IS-E2E-26.client-a.stderr"
  local pid_a="${last_http_pid}"
  post_json_async "${CLIENT_B_HTTP}/concurrent" \
    "{\"spotId\":\"${spot_id}\",\"count\":6,\"operationPrefix\":\"op-26-b-${run_id}\",\"timeoutMilliseconds\":10000}" \
    "${path_b}" "${log_dir}/IS-E2E-26.client-b.stderr"
  local pid_b="${last_http_pid}"
  wait_http_result "${pid_a}" "${path_a}" 240 || { wait "${pid_a}" || true; return 1; }
  wait_http_result "${pid_b}" "${path_b}" 240 || { wait "${pid_b}" || true; return 1; }
  assert_concurrent "${path_a}" "${path_b}" "${spot_id}"
  operations="$(python3 - "${path_a}" "${path_b}" <<'PY'
import json
import sys
operations = []
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as stream:
        operations.extend(
            reply["reply"]["operationId"]
            for reply in json.load(stream)["outcomes"])
print(",".join(operations))
PY
)"
  snapshot_evidence "IS-E2E-26"
  assert_single_activation "${spot_id}" "${operations}" \
    "${log_dir}/IS-E2E-26-owner-a.evidence.json" \
    "${log_dir}/IS-E2E-26-owner-b.evidence.json"
  echo "PASS IS-E2E-26 process=owner-a/owner-b client=client-a+client-b wire=single-claim-convergence"
}

scenario_31() {
  local spot_id="remote-selection-${run_id}"
  local operation_a="op-31-a-${run_id}"
  local operation_b="op-31-b-${run_id}"
  local path_a="${log_dir}/IS-E2E-31.client-a.json"
  local path_b="${log_dir}/IS-E2E-31.client-b.json"
  post_json_async "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${operation_a}\",\"payload\":\"remote-selection-a\",\"timeoutMilliseconds\":10000}" \
    "${path_a}" "${log_dir}/IS-E2E-31.client-a.stderr"
  local pid_a="${last_http_pid}"
  post_json_async "${CLIENT_B_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${operation_b}\",\"payload\":\"remote-selection-b\",\"timeoutMilliseconds\":10000}" \
    "${path_b}" "${log_dir}/IS-E2E-31.client-b.stderr"
  local pid_b="${last_http_pid}"
  wait_http_result "${pid_a}" "${path_a}" 120 || { wait "${pid_a}" || true; return 1; }
  wait_http_result "${pid_b}" "${path_b}" 120 || { wait "${pid_b}" || true; return 1; }
  assert_request_success "${path_a}" "${spot_id}" "${operation_a}"
  assert_request_success "${path_b}" "${spot_id}" "${operation_b}"
  snapshot_evidence "IS-E2E-31"
  assert_single_activation "${spot_id}" "${operation_a},${operation_b}" \
    "${log_dir}/IS-E2E-31-owner-a.evidence.json" \
    "${log_dir}/IS-E2E-31-owner-b.evidence.json"
  python3 "${log_dir}/IS-E2E-31-owner-a.evidence.json" \
    "${log_dir}/IS-E2E-31-owner-b.evidence.json" "${spot_id}" <<'PY'
import json
import sys
events = []
for path in sys.argv[1:3]:
    with open(path, encoding="utf-8") as stream:
        events.extend(json.load(stream)["events"])
factory_owners = {event["ownerRid"] for event in events
                  if event["kind"] == "FACTORY" and event["spotId"] == sys.argv[3]}
if len(factory_owners) != 1:
    raise SystemExit(f"remote selection did not have one final owner: {factory_owners}")
PY
  echo "PASS IS-E2E-31 process=owner-a/owner-b client=client-a+client-b wire=single-selected-owner"
}

scenario_ready_owner_loss() {
  local scenario_id="$1"
  local spot_id="ready-owner-loss-${scenario_id}-${run_id}"
  local first_operation="${scenario_id}-first-${run_id}"
  local failed_operation="${scenario_id}-after-loss-${run_id}"
  local first_path="${log_dir}/${scenario_id}.first.json"
  local failed_path="${log_dir}/${scenario_id}.failed.json"
  post_json "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${first_operation}\",\"payload\":\"establish-ready-owner\",\"timeoutMilliseconds\":5000}" \
    "${first_path}"
  assert_request_success "${first_path}" "${spot_id}" "${first_operation}"
  snapshot_evidence "${scenario_id}-before-loss"
  local owner_rid owner_pid surviving_http
  owner_rid="$(python3 - "${first_path}" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(json.load(stream)["reply"]["ownerRid"])
PY
)"
  case "${owner_rid}" in
    owner-a) owner_pid="${OWNER_A_PID}"; surviving_http="${OWNER_B_HTTP}" ;;
    owner-b) owner_pid="${OWNER_B_PID}"; surviving_http="${OWNER_A_HTTP}" ;;
    *) echo "unexpected Ready owner RID: ${owner_rid}" >&2; return 1 ;;
  esac
  kill -9 "${owner_pid}"
  wait "${owner_pid}" >/dev/null 2>&1 || true
  sleep 3
  post_json "${CLIENT_B_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${failed_operation}\",\"payload\":\"must-not-reactivate\",\"timeoutMilliseconds\":3000}" \
    "${failed_path}"
  get_json "${surviving_http}/evidence" \
    "${log_dir}/${scenario_id}-surviving-owner.evidence.json"
  python3 - "${failed_path}" \
    "${log_dir}/${scenario_id}-before-loss-owner-a.evidence.json" \
    "${log_dir}/${scenario_id}-before-loss-owner-b.evidence.json" \
    "${log_dir}/${scenario_id}-surviving-owner.evidence.json" \
    "${spot_id}" "${failed_operation}" <<'PY'
import json
import sys

failure_path, before_a, before_b, after, spot_id, operation_id = sys.argv[1:]
with open(failure_path, encoding="utf-8") as stream:
    outcome = json.load(stream)
if outcome.get("succeeded") or outcome.get("errorKind") != "UNAVAILABLE":
    raise SystemExit(f"Ready owner loss was not bounded Unavailable: {outcome}")
events = []
for path in (before_a, before_b):
    with open(path, encoding="utf-8") as stream:
        events.extend(json.load(stream)["events"])
with open(after, encoding="utf-8") as stream:
    after_events = json.load(stream)["events"]
if sum(event["kind"] == "FACTORY" and event["spotId"] == spot_id
       for event in events) != 1:
    raise SystemExit("the initial Ready identity did not have exactly one factory")
if any(event["kind"] in ("FACTORY", "INITIALIZE")
       and event["spotId"] == spot_id for event in after_events):
    raise SystemExit("the surviving owner started forbidden cold activation")
if any(event["kind"] == "HANDLER_ENTER"
       and event["operationId"] == operation_id for event in after_events):
    raise SystemExit("the failed operation reached an application handler")
PY
  echo "PASS ${scenario_id} process=ready-owner-SIGKILL client=client-b terminal=UNAVAILABLE recovery=none"
}

scenario_05() {
  scenario_ready_owner_loss IS-E2E-05
}

scenario_35() {
  local scenario_id="IS-E2E-35"
  local spot_id="ready-pending-owner-loss-${run_id}"
  local ready_operation="${scenario_id}-ready-${run_id}"
  local pending_operation="${scenario_id}-pending-${run_id}"
  local queued_operation="${scenario_id}-queued-${run_id}"
  local failed_operation="${scenario_id}-after-loss-${run_id}"
  local gate_id="${scenario_id}-gate-${run_id}"
  local ready_path="${log_dir}/${scenario_id}.ready.json"
  local pending_path="${log_dir}/${scenario_id}.pending.json"
  local queued_path="${log_dir}/${scenario_id}.queued.json"
  local failed_path="${log_dir}/${scenario_id}.failed.json"

  post_json "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${ready_operation}\",\"payload\":\"establish-ready-owner\",\"timeoutMilliseconds\":5000}" \
    "${ready_path}"
  assert_request_success "${ready_path}" "${spot_id}" "${ready_operation}"

  local owner_rid owner_pid surviving_http
  owner_rid="$(python3 - "${ready_path}" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as stream:
    print(json.load(stream)["reply"]["ownerRid"])
PY
)"
  case "${owner_rid}" in
    owner-a) owner_pid="${OWNER_A_PID}"; surviving_http="${OWNER_B_HTTP}" ;;
    owner-b) owner_pid="${OWNER_B_PID}"; surviving_http="${OWNER_A_HTTP}" ;;
    *) echo "unexpected Ready owner RID: ${owner_rid}" >&2; return 1 ;;
  esac

  close_gate "${gate_id}"
  post_json_async "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${pending_operation}\",\"payload\":\"gate:${gate_id}\",\"timeoutMilliseconds\":10000}" \
    "${pending_path}" "${log_dir}/${scenario_id}.pending.stderr"
  local pending_pid="${last_http_pid}"
  wait_event_any HANDLER_ENTER "${pending_operation}" 8 || {
    open_gate "${gate_id}"
    wait "${pending_pid}" >/dev/null 2>&1 || true
    echo "Ready owner did not retain the gated pending request" >&2
    return 1
  }

  post_json_async "${CLIENT_B_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${queued_operation}\",\"payload\":\"gate:${gate_id}\",\"timeoutMilliseconds\":10000}" \
    "${queued_path}" "${log_dir}/${scenario_id}.queued.stderr"
  local queued_pid="${last_http_pid}"
  snapshot_evidence "${scenario_id}-before-loss"

  kill -9 "${owner_pid}"
  wait "${owner_pid}" >/dev/null 2>&1 || true
  wait_http_result "${pending_pid}" "${pending_path}" 30 || true
  wait_http_result "${queued_pid}" "${queued_path}" 30 || true
  wait "${pending_pid}" >/dev/null 2>&1 || true
  wait "${queued_pid}" >/dev/null 2>&1 || true
  sleep 3
  post_json "${CLIENT_B_HTTP}/request" \
    "{\"spotId\":\"${spot_id}\",\"operationId\":\"${failed_operation}\",\"payload\":\"must-not-reactivate\",\"timeoutMilliseconds\":3000}" \
    "${failed_path}"
  get_json "${surviving_http}/evidence" \
    "${log_dir}/${scenario_id}-surviving-owner.evidence.json"

  python3 - "${ready_path}" "${pending_path}" "${queued_path}" "${failed_path}" \
    "${log_dir}/${scenario_id}.pending.stderr" \
    "${log_dir}/${scenario_id}.queued.stderr" \
    "${log_dir}/${scenario_id}-before-loss-owner-a.evidence.json" \
    "${log_dir}/${scenario_id}-before-loss-owner-b.evidence.json" \
    "${log_dir}/${scenario_id}-surviving-owner.evidence.json" \
    "${spot_id}" "${pending_operation}" "${queued_operation}" "${failed_operation}" <<'PY'
import json
import sys

(ready_path, pending_path, queued_path, failed_path, pending_stderr, queued_stderr,
 before_a, before_b, after, spot_id, pending_operation, queued_operation,
 failed_operation) = sys.argv[1:]

def load(path):
    with open(path, encoding="utf-8") as stream:
        return json.load(stream)

ready = load(ready_path)
if not ready.get("succeeded"):
    raise SystemExit(f"Ready request failed: {ready}")
failed = load(failed_path)
if failed.get("succeeded") or failed.get("errorKind") != "UNAVAILABLE":
    raise SystemExit(f"post-loss request was not bounded Unavailable: {failed}")
for path, error_path in ((pending_path, pending_stderr), (queued_path, queued_stderr)):
    if not __import__("os").path.getsize(path):
        with open(error_path, encoding="utf-8") as stream:
            error = stream.read()
        if "curl:" not in error:
            raise SystemExit(f"pending request ended without terminal evidence: {path}")
        continue
    outcome = load(path)
    if outcome.get("succeeded"):
        raise SystemExit(f"owner-loss request unexpectedly succeeded: {outcome}")

before_events = load(before_a)["events"] + load(before_b)["events"]
after_events = load(after)["events"]
if sum(event["kind"] == "FACTORY" and event["spotId"] == spot_id
       for event in before_events) != 1:
    raise SystemExit("the Ready owner did not have exactly one factory")
if sum(event["kind"] == "INITIALIZE" and event["spotId"] == spot_id
       for event in before_events) != 1:
    raise SystemExit("the Ready owner did not have exactly one initialize")
entered = [event for event in before_events
           if event["kind"] == "HANDLER_ENTER"
           and event["spotId"] == spot_id
           and event["operationId"] in (pending_operation, queued_operation)]
if [event["operationId"] for event in entered] != [pending_operation]:
    raise SystemExit(f"pending queue was not retained behind the active handler: {entered}")
if any(event["operationId"] == queued_operation for event in entered):
    raise SystemExit("queued request entered the handler before owner loss")
if any(event["spotId"] == spot_id and event["kind"] in ("FACTORY", "INITIALIZE", "HANDLER_ENTER")
       for event in after_events):
    raise SystemExit("surviving owner performed forbidden automatic recovery")
if any(event["operationId"] == failed_operation for event in after_events):
    raise SystemExit("post-loss request reached an application handler")
PY
  echo "PASS ${scenario_id} process=ready-owner-SIGKILL client=client-a+client-b pending=active-handler+queued-request recovery=none"
}

run_scenario() {
  case "$1" in
    IS-E2E-01) scenario_01 ;;
    IS-E2E-02) scenario_02 ;;
    IS-E2E-03) scenario_03 ;;
    IS-E2E-04) scenario_04 ;;
    IS-E2E-08) scenario_08 ;;
    IS-E2E-19) scenario_19 ;;
    IS-E2E-26) scenario_26 ;;
    IS-E2E-31) scenario_31 ;;
    IS-E2E-05) scenario_05 ;;
    IS-E2E-35) scenario_35 ;;
    IS-E2E-06) blocked_scenario "$1" "fixture does not provide a factory-entry crash boundary and recovery controller" ;;
    IS-E2E-07) blocked_scenario "$1" "owner factory is explicitly configured with disableRelocation() for this fixture" ;;
    IS-E2E-09) blocked_scenario "$1" "fixture does not provide concurrent post-crash requests plus public lease invalidation evidence" ;;
    IS-E2E-10) blocked_scenario "$1" "fixture does not provide process pause/resume and stale-owner fencing control" ;;
    IS-E2E-11) blocked_scenario "$1" "fixture has no public admission-rejection topology or capacity controller" ;;
    IS-E2E-12) blocked_scenario "$1" "fixture has no network proxy that can drop a request after acceptance" ;;
    IS-E2E-13) blocked_scenario "$1" "fixture has no post-send process termination and replacement-owner orchestration" ;;
    IS-E2E-14) blocked_scenario "$1" "fixture has no controllable Redis outage/proxy while retaining public owner evidence" ;;
    IS-E2E-15) blocked_scenario "$1" "fixture registers only Instance Spot factories and has no User Spot contention process" ;;
    IS-E2E-16) blocked_scenario "$1" "fixture has no separate no-eligible-node and exhausted-capacity topologies" ;;
    IS-E2E-17) blocked_scenario "$1" "fixture does not expose a public activation-concurrency setting or factory gate topology" ;;
    IS-E2E-18) blocked_scenario "$1" "this Java fixture has no second Framework language process" ;;
    IS-E2E-20) blocked_scenario "$1" "fixture has no delayed close callback plus owner crash control" ;;
    IS-E2E-21) blocked_scenario "$1" "fixture configures one Mesh only; initial and follow-up Mesh routing cannot be compared" ;;
    IS-E2E-22) blocked_scenario "$1" "fixture has no process pause/resume control for monotonic deadline verification" ;;
    IS-E2E-23) blocked_scenario "$1" "fixture has no negative capability factory registration" ;;
    IS-E2E-24) blocked_scenario "$1" "fixture has no Location Store response-delay proxy" ;;
    IS-E2E-25) blocked_scenario "$1" "fixture has no one-shot initialize failure control" ;;
    IS-E2E-27) blocked_scenario "$1" "fixture has handler gates but no independent activation waiter/deadline control" ;;
    IS-E2E-28) blocked_scenario "$1" "fixture has no close-entry admission race controller" ;;
    IS-E2E-29) blocked_scenario "$1" "fixture disables relocation and has no cross-Mesh relocation process" ;;
    IS-E2E-30) blocked_scenario "$1" "fixture disables relocation and has no concurrent relocation controller" ;;
    IS-E2E-32) blocked_scenario "$1" "fixture has no separate activation crash-boundary orchestration" ;;
    IS-E2E-33) blocked_scenario "$1" "fixture has no factory/initialize failure injection" ;;
    IS-E2E-34) blocked_scenario "$1" "fixture has no target crash/restart control around unpublished activation" ;;
    IS-E2E-36) blocked_scenario "$1" "fixture has no before/after-handler crash injection" ;;
    *)
      echo "unknown InstanceSpot scenario: $1" >&2
      return 1
      ;;
  esac
}

record_status() {
  local id="$1"
  local status="$2"
  local reason="$3"
  printf '%s\t%s\t%s\n' "${id}" "${status}" "${reason}" >>"${status_file}"
  echo "[InstanceSpot] ${id} ${status} reason=${reason}"
}

run_one() {
  local id="$1"
  local output="${log_dir}/scenario-${id}.log"
  local code
  set +e
  (
    set -e
    run_scenario "${id}"
  ) > >(tee "${output}") 2>&1
  code="$?"
  set -e
  if [[ "${code}" == "0" ]]; then
    record_status "${id}" PASS "process/API/wire assertions completed"
  elif [[ "${code}" == "3" ]]; then
    reason="$(sed -n 's/^BLOCKED [^:]*: //p' "${output}" | tail -n 1)"
    [[ -n "${reason}" ]] || reason="see ${output}"
    record_status "${id}" BLOCKED "${reason}; evidence=${output}"
  else
    record_status "${id}" FAIL "assertion or process error; evidence=${output}"
    overall_failure=1
  fi
}

selected_ids=()
if [[ "${SCENARIO}" == "all" ]]; then
  selected_ids=("${scenario_ids[@]}")
else
  selected_ids=("${SCENARIO}")
fi
for id in "${selected_ids[@]}"; do
  if [[ ! " ${scenario_ids[*]} " =~ " ${id} " ]]; then
    echo "unknown InstanceSpot scenario: ${id}" >&2
    exit 1
  fi
done

echo "log_dir=${log_dir}"
echo "start_order=${START_ORDER}"
echo "scenario=${SCENARIO}"

overall_failure=0
set +e
setup_topology
setup_status="$?"
set -e
if [[ "${setup_status}" != "0" ]]; then
  if [[ "${setup_status}" == "3" ]]; then
    for id in "${selected_ids[@]}"; do
      record_status "${id}" BLOCKED "${topology_reason}; evidence=${log_dir}"
    done
    exit 3
  fi
  for id in "${selected_ids[@]}"; do
    record_status "${id}" FAIL "topology setup failed; evidence=${log_dir}"
  done
  exit 1
fi

for id in "${selected_ids[@]}"; do
  run_one "${id}"
done

if [[ "${overall_failure}" != "0" ]]; then
  exit 1
fi
if rg -n $'\tBLOCKED\t' "${status_file}" >/dev/null 2>&1; then
  exit 3
fi
exit 0
