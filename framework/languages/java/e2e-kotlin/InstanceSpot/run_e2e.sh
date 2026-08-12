#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$(cd "${ROOT}/../.." && pwd)/e2e-redis-common.sh"
zlink_e2e_initialize kotlin "$0" "$@"
cd "${ROOT}"
SCENARIO="${1:-all}"
RUN_ID="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="${ROOT}/logs/${RUN_ID}"
BUILD_DIR="${ZLINK_KOTLIN_E2E_BUILD_DIR:-${HOME}/.cache/zlink/kotlin-e2e/InstanceSpot}"
CACHE_DIR="${ZLINK_KOTLIN_E2E_GRADLE_CACHE:-${HOME}/.cache/zlink/kotlin-e2e/InstanceSpot-gradle-cache}"
CONFIG_DIR="$(mktemp -d)"
STATUS_FILE="${LOG_DIR}/scenario-status.tsv"
mkdir -p "${LOG_DIR}"
printf 'scenario\tstatus\treason\n' >"${STATUS_FILE}"

PIDS=()
REDIS_CONTAINER=""
REDIS_PORT=""
OWNER_A_HTTP=""
OWNER_A_PID=""
OWNER_B_HTTP=""
OWNER_B_PID=""
CLIENT_A_HTTP=""
CLIENT_B_HTTP=""
last_http_pid=""
overall_failure=0

cleanup() {
  local status="$?"
  set +e
  for pid in "${PIDS[@]}"; do kill "${pid}" >/dev/null 2>&1 || true; done
  sleep 0.2
  for pid in "${PIDS[@]}"; do kill -9 "${pid}" >/dev/null 2>&1 || true; done
  [[ -z "${REDIS_CONTAINER}" ]] || zlink_redis_remove_by_id "${REDIS_CONTAINER}" || true
  rm -rf "${CONFIG_DIR}"
  wait >/dev/null 2>&1 || true
  exit "${status}"
}
trap cleanup EXIT

ports() {
  zlink_e2e_reserve_ports "$1"
}

gradle_run() {
  zlink_e2e_gradle_build_locked env ZLINK_KOTLIN_E2E_BUILD_DIR="${BUILD_DIR}" \
    ../../gradlew --project-cache-dir "${CACHE_DIR}" --no-daemon "$@" --quiet
}
owner_bin() { echo "${BUILD_DIR}/Owner/install/instance-spot-kotlin-owner/bin/instance-spot-kotlin-owner"; }
client_bin() { echo "${BUILD_DIR}/Client/install/instance-spot-kotlin-client/bin/instance-spot-kotlin-client"; }
health() {
  local name="$1" endpoint="$2"
  for _ in $(seq 1 120); do
    curl --max-time 1 --fail --silent "${endpoint}/health" >"${LOG_DIR}/${name}.health.json" 2>"${LOG_DIR}/${name}.health.stderr" && return 0
    sleep 0.05
  done
  return 1
}

owner_config() {
  local rid="$1" lifecycle="$2" endpoint="$3" mesh="$4"
  cat >"${CONFIG_DIR}/${rid}.properties" <<EOF
e2e.rid=${rid}
e2e.lifecycle-id=${lifecycle}
e2e.http-endpoint=${endpoint}
e2e.mesh-endpoint=${mesh}
e2e.redis-location-endpoint=127.0.0.1:${REDIS_PORT}
e2e.location-key-prefix=zlink:e2e:kotlin-instance-spot:${RUN_ID}
e2e.redis-command-timeout-millis=500
e2e.heartbeat-millis=200
e2e.lease-ttl-millis=1500
e2e.polling-millis=100
e2e.store-failure-grace-millis=3000
e2e.stable-type-limit=0
e2e.disable-relocation=true
e2e.evidence-file=${LOG_DIR}/${rid}.evidence
e2e.log-dir=${LOG_DIR}
EOF
  chmod 600 "${CONFIG_DIR}/${rid}.properties"
}

client_config() {
  local rid="$1" endpoint="$2" mesh="$3"
  cat >"${CONFIG_DIR}/${rid}.properties" <<EOF
e2e.rid=${rid}
e2e.http-endpoint=${endpoint}
e2e.mesh-endpoint=${mesh}
e2e.redis-location-endpoint=127.0.0.1:${REDIS_PORT}
e2e.location-key-prefix=zlink:e2e:kotlin-instance-spot:${RUN_ID}
e2e.redis-command-timeout-millis=500
e2e.heartbeat-millis=200
e2e.lease-ttl-millis=1500
e2e.polling-millis=100
e2e.store-failure-grace-millis=3000
e2e.log-dir=${LOG_DIR}
EOF
  chmod 600 "${CONFIG_DIR}/${rid}.properties"
}

start_owner() {
  owner_config "$1" "$2" "$3" "$4"
  "$(owner_bin)" --e2e-config "${CONFIG_DIR}/$1.properties" >"${LOG_DIR}/$1.stdout.log" 2>"${LOG_DIR}/$1.stderr.log" &
  local pid="$!"
  PIDS+=("${pid}")
  if [[ "$1" == "owner-a" ]]; then OWNER_A_PID="${pid}"; else OWNER_B_PID="${pid}"; fi
  health "$1" "$3"
}
start_client() {
  client_config "$1" "$2" "$3"
  "$(client_bin)" --e2e-config "${CONFIG_DIR}/$1.properties" >"${LOG_DIR}/$1.stdout.log" 2>"${LOG_DIR}/$1.stderr.log" &
  PIDS+=("$!"); health "$1" "$2"
}

setup() {
  command -v docker >/dev/null 2>&1 || { echo "Docker is required for Redis Location Store" >&2; return 3; }
  if [[ "${ZLINK_E2E_REBUILD:-0}" == "1" || ! -x "$(owner_bin)" || ! -x "$(client_bin)" ]]; then
    local -a build_args=(:Owner:installDist :Client:installDist)
    if [[ "${ZLINK_E2E_REBUILD:-0}" == "1" ]]; then
      build_args=(--refresh-dependencies clean "${build_args[@]}")
    fi
    gradle_run "${build_args[@]}"
  fi
  zlink_redis_start_scoped_assign REDIS_CONTAINER REDIS_PORT \
    "zlink-redis-kotlin-instance-spot" "redis:7.2-alpine" || return 3
  read -r oh1 om1 oh2 om2 ch1 cm1 ch2 cm2 <<<"$(ports 8)"
  OWNER_A_HTTP="http://127.0.0.1:${oh1}"; OWNER_B_HTTP="http://127.0.0.1:${oh2}"
  CLIENT_A_HTTP="http://127.0.0.1:${ch1}"; CLIENT_B_HTTP="http://127.0.0.1:${ch2}"
  start_owner owner-a "${RUN_ID}-owner-a" "${OWNER_A_HTTP}" "tcp://127.0.0.1:${om1}"
  start_owner owner-b "${RUN_ID}-owner-b" "${OWNER_B_HTTP}" "tcp://127.0.0.1:${om2}"
  start_client client-a "${CLIENT_A_HTTP}" "tcp://127.0.0.1:${cm1}"
  start_client client-b "${CLIENT_B_HTTP}" "tcp://127.0.0.1:${cm2}"
}

get_json() { curl --max-time 8 --fail --silent --show-error "$1" >"$2"; }
post_json() { curl --max-time 12 --fail --silent --show-error -H 'Content-Type: application/json' -X POST "$1" --data "$2" >"$3"; }
post_async() { curl --max-time 240 --fail --silent --show-error -H 'Content-Type: application/json' -X POST "$1" --data "$2" >"$3" 2>"$4" & last_http_pid="$!"; }
valid_json() { [[ -s "$1" ]] && python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$1"; }
wait_result() {
  local pid="$1" path="$2" seconds="$3"
  local deadline=$((SECONDS + seconds))
  while ((SECONDS < deadline)); do
    valid_json "${path}" && return 0
    kill -0 "${pid}" >/dev/null 2>&1 || { wait "${pid}" || true; return 1; }
    sleep 0.05
  done
  return 1
}
set_gate() {
  local id="$1" open="$2" body="{\"gateId\":\"$1\",\"open\":$2}"
  post_json "${OWNER_A_HTTP}/gate" "${body}" "${LOG_DIR}/gate-a-${id}-${open}.json"
  post_json "${OWNER_B_HTTP}/gate" "${body}" "${LOG_DIR}/gate-b-${id}-${open}.json"
}
snapshot() {
  get_json "${OWNER_A_HTTP}/evidence" "${LOG_DIR}/$1-owner-a.evidence.json"
  get_json "${OWNER_B_HTTP}/evidence" "${LOG_DIR}/$1-owner-b.evidence.json"
}
wait_event() {
  local kind="$1" op="$2" seconds="$3"
  local deadline=$((SECONDS + seconds))
  while ((SECONDS < deadline)); do
    for endpoint in "${OWNER_A_HTTP}" "${OWNER_B_HTTP}"; do
      local path="${LOG_DIR}/wait-${kind}-${op}.json"
      if get_json "${endpoint}/evidence" "${path}" && python3 - "${path}" "${kind}" "${op}" 2>/dev/null <<'PY'
import json,sys
events=json.load(open(sys.argv[1]))["events"]
assert any(e["kind"]==sys.argv[2] and e["operationId"]==sys.argv[3] for e in events)
PY
      then return 0; fi
    done
    sleep 0.05
  done
  return 1
}
assert_reply() {
  python3 - "$1" "$2" "$3" <<'PY'
import json,sys
r=json.load(open(sys.argv[1])); assert r["succeeded"], r
reply=r["reply"]; assert reply["spotId"]==sys.argv[2] and reply["operationId"]==sys.argv[3]
assert reply["ownerRid"] and reply["objectGeneration"]>0
PY
}
assert_activation() {
  python3 - "$@" <<'PY'
import json,sys
spot,ops,a,b=sys.argv[1:]
events=sum((json.load(open(p))["events"] for p in (a,b)), [])
assert len([e for e in events if e["kind"]=="FACTORY" and e["spotId"]==spot])==1
assert len([e for e in events if e["kind"]=="INITIALIZE" and e["spotId"]==spot])==1
for op in ops.split(','):
    enter=[e for e in events if e["kind"]=="HANDLER_ENTER" and e["spotId"]==spot and e["operationId"]==op]
    commit=[e for e in events if e["kind"]=="HANDLER_COMMIT" and e["spotId"]==spot and e["operationId"]==op]
    assert len(enter)==len(commit)==1, (op,enter,commit)
    assert enter[0]["activeHandlers"]<=1
PY
}

scenario_01() {
  local spot="cold-request-${RUN_ID}" op="op-01-${RUN_ID}" out="${LOG_DIR}/IS-E2E-01.json"
  post_json "${CLIENT_A_HTTP}/request" "{\"spotId\":\"${spot}\",\"operationId\":\"${op}\",\"payload\":\"cold-request\",\"timeoutMilliseconds\":5000}" "${out}"
  assert_reply "${out}" "${spot}" "${op}"
  post_json "${CLIENT_A_HTTP}/lookup" "{\"spotId\":\"${spot}\"}" "${LOG_DIR}/IS-E2E-01.lookup.json"
  python3 - "${LOG_DIR}/IS-E2E-01.lookup.json" "${spot}" <<'PY'
import json,sys
r=json.load(open(sys.argv[1])); assert r["found"] and r["spotId"]==sys.argv[2] and r["objectGeneration"]>0 and r["nodeRid"]
PY
  snapshot IS-E2E-01
  assert_activation "${spot}" "${op}" "${LOG_DIR}/IS-E2E-01-owner-a.evidence.json" "${LOG_DIR}/IS-E2E-01-owner-b.evidence.json"
  echo "PASS IS-E2E-01 process=owner-a+owner-b client=client-a API=typed-request+public-lookup"
}

scenario_02() {
  local spot="cold-send-${RUN_ID}" op="op-02-${RUN_ID}" gate="gate-02-${RUN_ID}" out="${LOG_DIR}/IS-E2E-02.json"
  set_gate "${gate}" false
  post_async "${CLIENT_A_HTTP}/send" "{\"spotId\":\"${spot}\",\"operationId\":\"${op}\",\"payload\":\"gate:${gate}\"}" "${out}" "${out}.stderr"
  local pid="${last_http_pid}"
  if wait_result "${pid}" "${out}" 20; then
    python3 - "${out}" <<'PY'
import json,sys
assert json.load(open(sys.argv[1]))["succeeded"]
PY
    snapshot IS-E2E-02-before-open
    if rg -n "\|SEND_HANDLER\|.*\|${op}\|" "${LOG_DIR}/owner-a.evidence" "${LOG_DIR}/owner-b.evidence" >/dev/null 2>&1; then
      set_gate "${gate}" true
      echo "BLOCKED IS-E2E-02: public send completion waited for gated handler"
      return 3
    fi
  fi
  set_gate "${gate}" true
  wait "${pid}" >/dev/null 2>&1 || true
  [[ -s "${out}" ]] || { echo "send result was not produced" >&2; return 1; }
  snapshot IS-E2E-02
  echo "PASS IS-E2E-02 process=owner-a+owner-b client=client-a API=one-way-submit+handler-evidence"
}

scenario_03() {
  local spot="concurrent-first-${RUN_ID}" a="${LOG_DIR}/IS-E2E-03.a.json" b="${LOG_DIR}/IS-E2E-03.b.json"
  post_async "${CLIENT_A_HTTP}/concurrent" "{\"spotId\":\"${spot}\",\"count\":8,\"operationPrefix\":\"op-03-a-${RUN_ID}\",\"timeoutMilliseconds\":10000}" "${a}" "${a}.stderr"; local pa="${last_http_pid}"
  post_async "${CLIENT_B_HTTP}/concurrent" "{\"spotId\":\"${spot}\",\"count\":8,\"operationPrefix\":\"op-03-b-${RUN_ID}\",\"timeoutMilliseconds\":10000}" "${b}" "${b}.stderr"; local pb="${last_http_pid}"
  wait_result "${pa}" "${a}" 240; wait_result "${pb}" "${b}" 240
  local operations="$(python3 - "${a}" "${b}" <<'PY'
import json,sys
ops=[]
for p in sys.argv[1:]:
    r=json.load(open(p)); assert all(x["succeeded"] for x in r["outcomes"]); ops += [x["reply"]["operationId"] for x in r["outcomes"]]
assert len(ops)==len(set(ops)); print(','.join(ops))
PY
)"
  snapshot IS-E2E-03
  assert_activation "${spot}" "${operations}" "${LOG_DIR}/IS-E2E-03-owner-a.evidence.json" "${LOG_DIR}/IS-E2E-03-owner-b.evidence.json"
  echo "PASS IS-E2E-03 process=owner-a+owner-b client=client-a+client-b API=16-concurrent-typed-requests"
}

scenario_04() {
  local spot_a="different-a-${RUN_ID}" spot_b="different-b-${RUN_ID}" op_a="op-04-a-${RUN_ID}" op_b="op-04-b-${RUN_ID}" gate="gate-04-${RUN_ID}"
  local a="${LOG_DIR}/IS-E2E-04.a.json" b="${LOG_DIR}/IS-E2E-04.b.json"
  set_gate "${gate}" false
  post_async "${CLIENT_A_HTTP}/request" "{\"spotId\":\"${spot_a}\",\"operationId\":\"${op_a}\",\"payload\":\"gate:${gate}\",\"timeoutMilliseconds\":10000}" "${a}" "${a}.stderr"; local pa="${last_http_pid}"
  wait_event HANDLER_ENTER "${op_a}" 8
  post_async "${CLIENT_B_HTTP}/request" "{\"spotId\":\"${spot_b}\",\"operationId\":\"${op_b}\",\"payload\":\"independent\",\"timeoutMilliseconds\":5000}" "${b}" "${b}.stderr"; local pb="${last_http_pid}"
  wait_result "${pb}" "${b}" 60; assert_reply "${b}" "${spot_b}" "${op_b}"
  set_gate "${gate}" true; wait "${pa}" >/dev/null 2>&1 || true; assert_reply "${a}" "${spot_a}" "${op_a}"
  snapshot IS-E2E-04
  assert_activation "${spot_a}" "${op_a}" "${LOG_DIR}/IS-E2E-04-owner-a.evidence.json" "${LOG_DIR}/IS-E2E-04-owner-b.evidence.json"
  assert_activation "${spot_b}" "${op_b}" "${LOG_DIR}/IS-E2E-04-owner-a.evidence.json" "${LOG_DIR}/IS-E2E-04-owner-b.evidence.json"
  echo "PASS IS-E2E-04 process=owner-a+owner-b client=client-a+client-b API=independent-Spot-progress"
}

scenario_08() {
  local spot="close-reactivate-${RUN_ID}" first="op-08-first-${RUN_ID}" close="op-08-close-${RUN_ID}" second="op-08-second-${RUN_ID}"
  local a="${LOG_DIR}/IS-E2E-08.first.json" c="${LOG_DIR}/IS-E2E-08.close.json" b="${LOG_DIR}/IS-E2E-08.second.json"
  post_json "${CLIENT_A_HTTP}/request" "{\"spotId\":\"${spot}\",\"operationId\":\"${first}\",\"payload\":\"before-close\",\"timeoutMilliseconds\":5000}" "${a}"
  assert_reply "${a}" "${spot}" "${first}"
  local generation="$(python3 - "${a}" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))["reply"]["objectGeneration"])
PY
)"
  post_json "${CLIENT_A_HTTP}/close" "{\"spotId\":\"${spot}\",\"operationId\":\"${close}\",\"gateId\":\"\"}" "${c}"
  wait_event CLOSE_RESULT "${close}" 15
  post_json "${CLIENT_A_HTTP}/request" "{\"spotId\":\"${spot}\",\"operationId\":\"${second}\",\"payload\":\"after-close\",\"timeoutMilliseconds\":5000}" "${b}"
  assert_reply "${b}" "${spot}" "${second}"
  local next_generation="$(python3 - "${b}" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))["reply"]["objectGeneration"])
PY
)"
  [[ "${generation}" != "${next_generation}" ]] || { echo "close reused generation" >&2; return 1; }
  snapshot IS-E2E-08
  python3 "${LOG_DIR}/IS-E2E-08-owner-a.evidence.json" "${LOG_DIR}/IS-E2E-08-owner-b.evidence.json" "${spot}" "${first}" "${second}" <<'PY'
import json,sys
events=sum((json.load(open(p))["events"] for p in sys.argv[1:3]), [])
spot,first,second=sys.argv[3:]
factories=[e for e in events if e["kind"]=="FACTORY" and e["spotId"]==spot]
initializes=[e for e in events if e["kind"]=="INITIALIZE" and e["spotId"]==spot]
assert len(factories)==len(initializes)==2
assert len({e["objectGeneration"] for e in factories})==2
assert len([e for e in events if e["kind"]=="CLOSE_RESULT" and e["spotId"]==spot])==1
for op in (first,second):
    assert len([e for e in events if e["kind"]=="HANDLER_COMMIT" and e["operationId"]==op])==1
PY
  echo "PASS IS-E2E-08 process=owner-a+owner-b client=client-a API=public-close+new-generation"
}

scenario_19() {
  local spot="ready-ordering-${RUN_ID}" first="op-19-first-${RUN_ID}" follow="op-19-follow-${RUN_ID}" gate="gate-19-${RUN_ID}"
  local a="${LOG_DIR}/IS-E2E-19.first.json" b="${LOG_DIR}/IS-E2E-19.follow.json"
  set_gate "${gate}" false
  post_async "${CLIENT_A_HTTP}/request" "{\"spotId\":\"${spot}\",\"operationId\":\"${first}\",\"payload\":\"gate:${gate}\",\"timeoutMilliseconds\":10000}" "${a}" "${a}.stderr"
  local pa="${last_http_pid}"; wait_event HANDLER_ENTER "${first}" 8
  post_async "${CLIENT_B_HTTP}/request" "{\"spotId\":\"${spot}\",\"operationId\":\"${follow}\",\"payload\":\"follow-up\",\"timeoutMilliseconds\":30000}" "${b}" "${b}.stderr"
  local pb="${last_http_pid}"
  if wait_result "${pb}" "${b}" 5; then set_gate "${gate}" true; return 1; fi
  set_gate "${gate}" true; wait "${pa}" >/dev/null 2>&1 || true; wait "${pb}" >/dev/null 2>&1 || true
  assert_reply "${a}" "${spot}" "${first}"; assert_reply "${b}" "${spot}" "${follow}"
  snapshot IS-E2E-19
  python3 "${LOG_DIR}/IS-E2E-19-owner-a.evidence.json" "${LOG_DIR}/IS-E2E-19-owner-b.evidence.json" "${spot}" "${first}" "${follow}" <<'PY'
import json,sys
events=sum((json.load(open(p))["events"] for p in sys.argv[1:3]), [])
spot,first,follow=sys.argv[3:]
ordered=[e["operationId"] for e in sorted(events,key=lambda e:e["sequence"]) if e["kind"]=="HANDLER_ENTER" and e["spotId"]==spot]
assert ordered[:2]==[first,follow], ordered
PY
  echo "PASS IS-E2E-19 process=owner-a+owner-b client=client-a+client-b API=queue-order-before-follow-up"
}

scenario_26() {
  local spot="concurrent-claim-${RUN_ID}" a="${LOG_DIR}/IS-E2E-26.a.json" b="${LOG_DIR}/IS-E2E-26.b.json"
  post_async "${CLIENT_A_HTTP}/concurrent" "{\"spotId\":\"${spot}\",\"count\":6,\"operationPrefix\":\"op-26-a-${RUN_ID}\",\"timeoutMilliseconds\":10000}" "${a}" "${a}.stderr"; local pa="${last_http_pid}"
  post_async "${CLIENT_B_HTTP}/concurrent" "{\"spotId\":\"${spot}\",\"count\":6,\"operationPrefix\":\"op-26-b-${RUN_ID}\",\"timeoutMilliseconds\":10000}" "${b}" "${b}.stderr"; local pb="${last_http_pid}"
  wait_result "${pa}" "${a}" 240; wait_result "${pb}" "${b}" 240
  local operations="$(python3 - "${a}" "${b}" <<'PY'
import json,sys
ops=[]
for p in sys.argv[1:]:
    result=json.load(open(p)); assert all(x["succeeded"] for x in result["outcomes"])
    ops += [x["reply"]["operationId"] for x in result["outcomes"]]
assert len(ops)==len(set(ops)); print(','.join(ops))
PY
)"
  snapshot IS-E2E-26
  assert_activation "${spot}" "${operations}" "${LOG_DIR}/IS-E2E-26-owner-a.evidence.json" "${LOG_DIR}/IS-E2E-26-owner-b.evidence.json"
  echo "PASS IS-E2E-26 process=owner-a+owner-b client=client-a+client-b API=single-claim-convergence"
}

scenario_31() {
  local spot="remote-selection-${RUN_ID}" aop="op-31-a-${RUN_ID}" bop="op-31-b-${RUN_ID}" a="${LOG_DIR}/IS-E2E-31.a.json" b="${LOG_DIR}/IS-E2E-31.b.json"
  post_async "${CLIENT_A_HTTP}/request" "{\"spotId\":\"${spot}\",\"operationId\":\"${aop}\",\"payload\":\"remote-a\",\"timeoutMilliseconds\":10000}" "${a}" "${a}.stderr"; local pa="${last_http_pid}"
  post_async "${CLIENT_B_HTTP}/request" "{\"spotId\":\"${spot}\",\"operationId\":\"${bop}\",\"payload\":\"remote-b\",\"timeoutMilliseconds\":10000}" "${b}" "${b}.stderr"; local pb="${last_http_pid}"
  wait_result "${pa}" "${a}" 120; wait_result "${pb}" "${b}" 120
  assert_reply "${a}" "${spot}" "${aop}"; assert_reply "${b}" "${spot}" "${bop}"
  snapshot IS-E2E-31
  assert_activation "${spot}" "${aop},${bop}" "${LOG_DIR}/IS-E2E-31-owner-a.evidence.json" "${LOG_DIR}/IS-E2E-31-owner-b.evidence.json"
  echo "PASS IS-E2E-31 process=owner-a+owner-b client=client-a+client-b API=single-selected-owner"
}

scenario_ready_owner_loss() {
  local id="$1" spot="ready-owner-loss-${1}-${RUN_ID}"
  local first="${1}-first-${RUN_ID}" failed="${1}-after-loss-${RUN_ID}"
  local first_path="${LOG_DIR}/${1}.first.json" failed_path="${LOG_DIR}/${1}.failed.json"
  post_json "${CLIENT_A_HTTP}/request" \
    "{\"spotId\":\"${spot}\",\"operationId\":\"${first}\",\"payload\":\"establish-ready-owner\",\"timeoutMilliseconds\":5000}" \
    "${first_path}"
  assert_reply "${first_path}" "${spot}" "${first}"
  snapshot "${id}-before-loss"
  local owner owner_pid surviving_http
  owner="$(python3 - "${first_path}" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))["reply"]["ownerRid"])
PY
)"
  case "${owner}" in
    owner-a) owner_pid="${OWNER_A_PID}"; surviving_http="${OWNER_B_HTTP}";;
    owner-b) owner_pid="${OWNER_B_PID}"; surviving_http="${OWNER_A_HTTP}";;
    *) echo "unexpected Ready owner RID: ${owner}" >&2; return 1;;
  esac
  kill -9 "${owner_pid}"
  wait "${owner_pid}" >/dev/null 2>&1 || true
  sleep 3
  post_json "${CLIENT_B_HTTP}/request" \
    "{\"spotId\":\"${spot}\",\"operationId\":\"${failed}\",\"payload\":\"must-not-reactivate\",\"timeoutMilliseconds\":3000}" \
    "${failed_path}"
  get_json "${surviving_http}/evidence" "${LOG_DIR}/${id}-surviving-owner.evidence.json"
  python3 - "${failed_path}" \
    "${LOG_DIR}/${id}-before-loss-owner-a.evidence.json" \
    "${LOG_DIR}/${id}-before-loss-owner-b.evidence.json" \
    "${LOG_DIR}/${id}-surviving-owner.evidence.json" "${spot}" "${failed}" <<'PY'
import json,sys
failure,before_a,before_b,after,spot,operation=sys.argv[1:]
outcome=json.load(open(failure))
assert not outcome["succeeded"] and outcome["errorKind"]=="UNAVAILABLE", outcome
events=sum((json.load(open(path))["events"] for path in (before_a,before_b)), [])
after_events=json.load(open(after))["events"]
assert len([e for e in events if e["kind"]=="FACTORY" and e["spotId"]==spot])==1
assert not any(e["kind"] in ("FACTORY","INITIALIZE") and e["spotId"]==spot for e in after_events)
assert not any(e["kind"]=="HANDLER_ENTER" and e["operationId"]==operation for e in after_events)
PY
  echo "PASS ${id} process=ready-owner-SIGKILL client=client-b terminal=UNAVAILABLE recovery=none"
}

scenario_05() { scenario_ready_owner_loss IS-E2E-05; }
scenario_35() { scenario_ready_owner_loss IS-E2E-35; }

public_probe() {
  local id="$1" spot="probe-${id}-${RUN_ID}"
  curl --max-time 8 --fail --silent "${CLIENT_A_HTTP}/health" >"${LOG_DIR}/${id}.client-health.json"
  post_json "${CLIENT_A_HTTP}/lookup" "{\"spotId\":\"${spot}\"}" "${LOG_DIR}/${id}.lookup.json"
  snapshot "${id}"
}

blocked() {
  local id="$1" reason="$2"
  public_probe "${id}"
  echo "BLOCKED ${id}: ${reason}"
  printf '%s\n' "${reason}" >"${LOG_DIR}/${id}.blocked.txt"
  return 3
}

run_scenario() {
  case "$1" in
    IS-E2E-01) scenario_01;; IS-E2E-02) scenario_02;; IS-E2E-03) scenario_03;; IS-E2E-04) scenario_04;;
    IS-E2E-08) scenario_08;; IS-E2E-19) scenario_19;; IS-E2E-26) scenario_26;; IS-E2E-31) scenario_31;;
    IS-E2E-05) scenario_05;;
    IS-E2E-06) blocked "$1" "fixture has no factory-entry crash boundary controller";;
    IS-E2E-07) blocked "$1" "fixture explicitly disables relocation and has one Mesh";;
    IS-E2E-09) blocked "$1" "fixture has no ready-owner crash and lease invalidation sequence";;
    IS-E2E-10) blocked "$1" "fixture has no process pause/resume stale-owner control";;
    IS-E2E-11) blocked "$1" "fixture has no public admission rejection or capacity topology";;
    IS-E2E-12) blocked "$1" "fixture has no post-acceptance request-drop proxy";;
    IS-E2E-13) blocked "$1" "fixture has no accepted-send failure and replacement-owner orchestration";;
    IS-E2E-14) blocked "$1" "fixture has no controllable Redis outage proxy";;
    IS-E2E-15) blocked "$1" "fixture has no User Spot type-conflict process";;
    IS-E2E-16) blocked "$1" "fixture has no separate ineligible-node and exhausted-capacity topology";;
    IS-E2E-17) blocked "$1" "fixture has no public activation-concurrency control";;
    IS-E2E-18) blocked "$1" "fixture has no second Framework language process";;
    IS-E2E-20) blocked "$1" "fixture has no delayed close callback plus owner crash control";;
    IS-E2E-21) blocked "$1" "fixture has one Mesh and cannot compare initial/follow-up Mesh placement";;
    IS-E2E-22) blocked "$1" "fixture has no process pause/resume deadline control";;
    IS-E2E-23) blocked "$1" "fixture has no negative capability factory";;
    IS-E2E-24) blocked "$1" "fixture has no Location Store response-delay proxy";;
    IS-E2E-25) blocked "$1" "fixture has no one-shot initialize failure injection";;
    IS-E2E-27) blocked "$1" "fixture has no independent activation waiter/deadline controller";;
    IS-E2E-28) blocked "$1" "fixture has no close/admission race controller";;
    IS-E2E-29) blocked "$1" "fixture disables relocation and has no cross-Mesh relocation process";;
    IS-E2E-30) blocked "$1" "fixture disables relocation and has no concurrent relocation controller";;
    IS-E2E-32) blocked "$1" "fixture has no activation crash-boundary orchestration";;
    IS-E2E-33) blocked "$1" "fixture has no factory/initialize failure injection";;
    IS-E2E-34) blocked "$1" "fixture has no target crash control around unpublished activation";;
    IS-E2E-35) scenario_35;;
    IS-E2E-36) blocked "$1" "fixture has no before/after-handler crash injection";;
    *) echo "unknown InstanceSpot selector: $1" >&2; return 1;;
  esac
}

record() {
  local id="$1" status="$2" reason="$3"
  printf '%s\t%s\t%s\n' "${id}" "${status}" "${reason}" >>"${STATUS_FILE}"
  echo "[InstanceSpot-Kotlin] ${id} ${status} reason=${reason}"
}

scenario_ids=(
  IS-E2E-01 IS-E2E-02 IS-E2E-03 IS-E2E-04 IS-E2E-05 IS-E2E-06 IS-E2E-07 IS-E2E-08 IS-E2E-09 IS-E2E-10 IS-E2E-11 IS-E2E-12
  IS-E2E-13 IS-E2E-14 IS-E2E-15 IS-E2E-16 IS-E2E-17 IS-E2E-18 IS-E2E-19 IS-E2E-20 IS-E2E-21 IS-E2E-22 IS-E2E-23 IS-E2E-24
  IS-E2E-25 IS-E2E-26 IS-E2E-27 IS-E2E-28 IS-E2E-29 IS-E2E-30 IS-E2E-31 IS-E2E-32 IS-E2E-33 IS-E2E-34 IS-E2E-35 IS-E2E-36
)
selected=("${scenario_ids[@]}")
[[ "${SCENARIO}" == all ]] || selected=("${SCENARIO}")
for id in "${selected[@]}"; do [[ " ${scenario_ids[*]} " == *" ${id} "* ]] || { echo "unknown selector: ${id}" >&2; exit 1; }; done

echo "log_dir=${LOG_DIR}"; echo "scenario=${SCENARIO}"
if ! setup; then
  for id in "${selected[@]}"; do record "${id}" BLOCKED "topology unavailable; evidence=${LOG_DIR}"; done
  exit 3
fi

for id in "${selected[@]}"; do
  output="${LOG_DIR}/scenario-${id}.log"
  set +e
  (set -e; run_scenario "${id}") > >(tee "${output}") 2>&1
  code=$?
  set -e
  if [[ "${code}" == 0 ]]; then
    record "${id}" PASS "process/API/wire assertions completed"
  elif [[ "${code}" == 3 ]]; then
    reason="$(sed -n 's/^BLOCKED [^:]*: //p' "${output}" | tail -n 1)"
    record "${id}" BLOCKED "${reason}; evidence=${output}"
  else
    record "${id}" FAIL "assertion/process error; evidence=${output}"
    overall_failure=1
  fi
done
[[ "${overall_failure}" == 0 ]] || exit 1
rg -n $'\tBLOCKED\t' "${STATUS_FILE}" >/dev/null 2>&1 && exit 3
exit 0
