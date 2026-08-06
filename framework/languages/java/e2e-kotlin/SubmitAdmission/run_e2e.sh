#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAVA_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
SCENARIO="${1:-all}"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$STAMP"
TEMP_DIR="$(mktemp -d)"
EVIDENCE_FILE="$LOG_DIR/evidence.jsonl"
mkdir -p "$LOG_DIR"

supported=(
  SA-E2E-01 SA-E2E-05 SA-E2E-08 SA-E2E-14
  SA-E2E-18 SA-E2E-19 SA-E2E-20
)
blocked=(
  SA-E2E-02 SA-E2E-03 SA-E2E-04 SA-E2E-06 SA-E2E-07
  SA-E2E-09 SA-E2E-10 SA-E2E-11 SA-E2E-12 SA-E2E-13
  SA-E2E-15 SA-E2E-16 SA-E2E-17
)
variants=(SA-E2E-06-shutdown)

declare -A blocker_reason=(
  [SA-E2E-02]="public paused=true is observed, but remote Node await completes Submitted instead of pending; framework/core admission change is required"
  [SA-E2E-03]="both HWM targets accept the operation immediately instead of producing success/deadline pending terminals; framework/core admission change is required"
  [SA-E2E-04]="the HWM operation completes Submitted before its deadline instead of DeadlineExceeded; framework/core admission change is required"
  [SA-E2E-06]="Shutdown seals admission (DRAINING/STOPPED with acceptingWork=false), but send returns raw MeshNode is not started; Relocate also needs a public Location/Relocation Store fixture"
  [SA-E2E-07]="the cancellation leg cannot become pending because remote Node admission completes Submitted; Logical Multicast commit fixture is also absent"
  [SA-E2E-09]="the basic Channel path exists, but the common HWM/deadline variants complete Submitted before pending; framework/core admission change is required"
  [SA-E2E-10]="ClientServer blocker reaches paused=true, but the follow-up operation does not remain pending and the client process loses its HTTP response; framework/core admission change is required"
  [SA-E2E-11]="no Spot owner generation and route-loss fixture in this bounded suite"
  [SA-E2E-12]="no Actor owner generation and route-loss fixture in this bounded suite"
  [SA-E2E-13]="no multicast target snapshot barrier or executor direct-handoff fixture"
  [SA-E2E-15]="no bound Session and Session Actor gateway fixture"
  [SA-E2E-16]="no public STREAM peer fixture with wire-order evidence"
  [SA-E2E-17]="no request reply-token fixture with concurrent terminal barrier"
)

pids=()
target_pid=""
ROLE_BIN=""
declare -A role_pids=()

contains() {
  local expected="$1"
  shift
  local current
  for current in "$@"; do
    [[ "$expected" == "$current" ]] && return 0
  done
  return 1
}

is_known() {
  contains "$1" "${supported[@]}" \
    || contains "$1" "${blocked[@]}" \
    || contains "$1" "${variants[@]}"
}

stop_pid() {
  local pid="$1"
  [[ -n "$pid" ]] || return 0
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid" 2>/dev/null || true
    return 0
  fi
  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 20); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.05
  done
  if kill -0 "$pid" 2>/dev/null; then
    kill -KILL "$pid" 2>/dev/null || true
  fi
  wait "$pid" 2>/dev/null || true
}

cleanup() {
  local code=$?
  local pid log
  for pid in "${pids[@]}"; do
    stop_pid "$pid"
  done
  rm -rf "$TEMP_DIR"
  if [[ "$code" -ne 0 ]]; then
    for log in "$LOG_DIR"/*.stderr.log; do
      [[ -f "$log" ]] || continue
      echo "==> $log" >&2
      tail -80 "$log" >&2 || true
    done
  fi
}
trap cleanup EXIT

allocate_ports() {
  python3 - <<'PY'
import socket

ports = []
for _ in range(7):
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    ports.append(sock.getsockname()[1])
    sock.close()
print(*ports)
PY
}

build_role() {
  "$JAVA_ROOT/gradlew" --no-daemon --no-parallel \
    -p "$SCRIPT_DIR" :Role:installDist
  ROLE_BIN="$SCRIPT_DIR/Role/build/install/submit-admission-kotlin-role/bin/submit-admission-kotlin-role"
}

start_role() {
  local name="$1"
  shift
  "$ROLE_BIN" "$@" \
    >"$LOG_DIR/${name}.stdout.log" 2>"$LOG_DIR/${name}.stderr.log" &
  local pid=$!
  role_pids["$name"]="$pid"
  pids+=("$pid")
}

stop_role() {
  local name="$1"
  local pid="${role_pids[$name]:-}"
  [[ -n "$pid" ]] || return 0
  stop_pid "$pid"
  role_pids["$name"]=""
}

wait_health() {
  local url="$1" pid="$2" label="$3"
  for _ in $(seq 1 200); do
    if ! kill -0 "$pid" 2>/dev/null; then
      echo "$label process exited before health readiness" >&2
      return 1
    fi
    if curl --max-time 1 -fsS "$url/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "$label health readiness exceeded 20 seconds" >&2
  return 1
}

wait_route_ready() {
  local url="$1" target_rid="$2"
  for _ in $(seq 1 50); do
    if curl --max-time 1 -fsS "$url/ready?targetRid=$target_rid" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "target route did not become ready within 5 seconds" >&2
  return 1
}

wait_route_disconnected() {
  local url="$1" target_rid="$2"
  for _ in $(seq 1 200); do
    if ! curl --max-time 1 -fsS "$url/ready?targetRid=$target_rid" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "target route remained ready 20 seconds after target exit" >&2
  return 1
}

expect_status() {
  local expected="$1" url="$2" actual
  actual="$(curl --max-time 3 -fsS "$url")"
  if [[ "$actual" != "$expected" ]]; then
    echo "expected status=$expected actual=$actual url=$url" >&2
    return 1
  fi
}

wait_counts() {
  local url="$1" expected_started="$2" expected_completed="$3" actual=""
  for _ in $(seq 1 50); do
    actual="$(curl --max-time 1 -fsS "$url/counts")"
    if [[ "$actual" == "started=$expected_started,completed=$expected_completed" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected counts=started=$expected_started,completed=$expected_completed actual=$actual" >&2
  return 1
}

wait_handler_started() {
  local url="$1" operation_id="$2" actual=""
  for _ in $(seq 1 100); do
    actual="$(curl --max-time 1 -fsS \
      "$url/handler-counts?operationId=$operation_id")"
    if [[ "$actual" == started=1,* ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "handler did not start operation=$operation_id actual=$actual" >&2
  return 1
}

wait_handler_counts() {
  local url="$1" operation_id="$2" expected="$3" actual=""
  for _ in $(seq 1 100); do
    actual="$(curl --max-time 1 -fsS \
      "$url/handler-counts?operationId=$operation_id")"
    if [[ "$actual" == "$expected" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected handler counts=$expected operation=$operation_id actual=$actual" >&2
  return 1
}

wait_operation() {
  local url="$1" operation_id="$2" expected="$3" actual=""
  for _ in $(seq 1 120); do
    actual="$(curl --max-time 1 -fsS \
      "$url/operation?operationId=$operation_id")"
    if [[ "$actual" == "$expected" ]]; then
      return 0
    fi
    sleep 0.05
  done
  echo "expected operation=$operation_id status=$expected actual=$actual" >&2
  return 1
}

assert_operation_pending() {
  local url="$1" operation_id="$2" actual
  sleep 0.2
  actual="$(curl --max-time 1 -fsS \
    "$url/operation?operationId=$operation_id")"
  if [[ "$actual" != RUNNING && "$actual" != PENDING ]]; then
    echo "operation was not pending operation=$operation_id status=$actual" >&2
    return 1
  fi
}

wait_runtime_paused() {
  local url="$1" actual=""
  for _ in $(seq 1 100); do
    actual="$(curl --max-time 1 -fsS "$url/runtime-status")"
    if [[ "$actual" == *"paused=true"* ]]; then
      echo "runtime-status $actual"
      return 0
    fi
    sleep 0.1
  done
  echo "application receive did not become paused actual=$actual" >&2
  return 1
}

wait_runtime_not_accepting() {
  local url="$1" actual=""
  for _ in $(seq 1 100); do
    actual="$(curl --max-time 1 -fsS "$url/runtime-status")"
    if [[ "$actual" == *"acceptingWork=false"* ]]; then
      echo "runtime-status $actual"
      return 0
    fi
    sleep 0.1
  done
  echo "runtime did not close admission actual=$actual" >&2
  return 1
}

wait_client_server_ready() {
  local url="$1" actual=""
  for _ in $(seq 1 100); do
    actual="$(curl --max-time 1 -fsS "$url/clientserver-ready" || true)"
    if [[ "$actual" == true ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "ClientServer target did not become ready actual=$actual" >&2
  return 1
}

expect_body() {
  local expected="$1" url="$2" actual=""
  actual="$(curl --max-time 5 -sS "$url" || true)"
  if [[ "$actual" != "$expected" ]]; then
    echo "expected body=$expected actual=$actual url=$url" >&2
    return 1
  fi
}

large_payload() {
  python3 - <<'PY'
print("x" * 4096, end="")
PY
}

record_pass() {
  local scenario_id="$1" details="$2"
  printf '{"scenarioId":"%s","status":"PASS",%s}\n' \
    "$scenario_id" "$details" >>"$EVIDENCE_FILE"
}

run_process_scenarios() {
  local selectors=("$@")
  local caller_http target_http publisher_http subscriber_http caller_mesh target_mesh fanout_port
  read -r caller_http target_http publisher_http subscriber_http caller_mesh target_mesh fanout_port \
    < <(allocate_ports)
  local caller_url="http://127.0.0.1:$caller_http"
  local target_url="http://127.0.0.1:$target_http"
  local publisher_url="http://127.0.0.1:$publisher_http"
  local subscriber_url="http://127.0.0.1:$subscriber_http"
  local gate="$TEMP_DIR/handler-gate.open"
  local role_bin="$SCRIPT_DIR/Role/build/install/submit-admission-kotlin-role/bin/submit-admission-kotlin-role"

  "$role_bin" \
    --role=target --rid=submit-target --httpPort="$target_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_mesh" \
    --gateFile="$gate" --evidenceFile="$LOG_DIR/target-evidence.jsonl" \
    >"$LOG_DIR/target.stdout.log" 2>"$LOG_DIR/target.stderr.log" &
  target_pid=$!
  pids+=("$target_pid")

  "$role_bin" \
    --role=caller --rid=submit-caller --httpPort="$caller_http" \
    --meshEndpoint="tcp://127.0.0.1:$caller_mesh" \
    --peerRid=submit-target --peerEndpoint="tcp://127.0.0.1:$target_mesh" \
    --gateFile="$gate" --evidenceFile="$LOG_DIR/caller-evidence.jsonl" \
    >"$LOG_DIR/caller.stdout.log" 2>"$LOG_DIR/caller.stderr.log" &
  local caller_pid=$!
  pids+=("$caller_pid")

  "$role_bin" \
    --role=publisher --rid=submit-publisher --httpPort="$publisher_http" \
    --fanoutEndpoint="tcp://127.0.0.1:$fanout_port" \
    --evidenceFile="$LOG_DIR/publisher-evidence.jsonl" \
    >"$LOG_DIR/publisher.stdout.log" 2>"$LOG_DIR/publisher.stderr.log" &
  local publisher_pid=$!
  pids+=("$publisher_pid")

  wait_health "$target_url" "$target_pid" target
  wait_health "$caller_url" "$caller_pid" caller
  wait_health "$publisher_url" "$publisher_pid" publisher
  wait_route_ready "$caller_url" submit-target

  local target_expected=0 caller_expected=0
  if contains SA-E2E-01 "${selectors[@]}"; then
    expect_status Submitted \
      "$caller_url/send-node?targetRid=submit-target&operationId=fast-node&sequence=1"
    expect_status Submitted \
      "$caller_url/send-channel?operationId=fast-channel&sequence=2"
    target_expected=$((target_expected + 2))
    wait_counts "$target_url" "$target_expected" "$target_expected"
    record_pass SA-E2E-01 '"publicKotlinAwaitCount":2,"remoteHandlerCount":2'
  fi

  if contains SA-E2E-08 "${selectors[@]}"; then
    expect_status Submitted \
      "$caller_url/send-node?targetRid=submit-caller&operationId=local-direct&sequence=3"
    expect_status Submitted \
      "$caller_url/send-node?targetRid=submit-target&operationId=remote-direct&sequence=4"
    caller_expected=$((caller_expected + 1))
    target_expected=$((target_expected + 1))
    wait_counts "$caller_url" "$caller_expected" "$caller_expected"
    wait_counts "$target_url" "$target_expected" "$target_expected"
    record_pass SA-E2E-08 '"localTerminal":"Submitted","remoteTerminal":"Submitted","handlerCountEach":1'
  fi

  if contains SA-E2E-09 "${selectors[@]}"; then
    expect_status Submitted \
      "$caller_url/send-channel?operationId=positive-weight-channel&sequence=5"
    target_expected=$((target_expected + 1))
    wait_counts "$target_url" "$target_expected" "$target_expected"
    record_pass SA-E2E-09 '"callerWeight":0,"targetWeight":100,"selectedTargetHandlers":1'
  fi

  if contains SA-E2E-14 "${selectors[@]}"; then
    expect_status Submitted \
      "$publisher_url/publish?operationId=subscriber-zero&sequence=6"
    "$role_bin" \
      --role=subscriber --rid=submit-subscriber --httpPort="$subscriber_http" \
      --fanoutEndpoint="tcp://127.0.0.1:$fanout_port" \
      --evidenceFile="$LOG_DIR/subscriber-evidence.jsonl" \
      >"$LOG_DIR/subscriber.stdout.log" 2>"$LOG_DIR/subscriber.stderr.log" &
    local subscriber_pid=$!
    pids+=("$subscriber_pid")
    wait_health "$subscriber_url" "$subscriber_pid" subscriber
    sleep 0.3
    wait_counts "$subscriber_url" 0 0
    stop_pid "$subscriber_pid"
    record_pass SA-E2E-14 '"subscriberCountAtSubmit":0,"lateSubscriberDeliveryCount":0'
  fi

  if contains SA-E2E-20 "${selectors[@]}"; then
    expect_status Submitted \
      "$caller_url/send-node?targetRid=submit-target&operationId=handler-gate-remote&sequence=7"
    target_expected=$((target_expected + 1))
    wait_counts "$target_url" "$target_expected" "$((target_expected - 1))"
    touch "$gate"
    wait_counts "$target_url" "$target_expected" "$target_expected"
    record_pass SA-E2E-20 '"submitBeforeHandlerCompletion":true,"handlerCount":1'
  fi

  if contains SA-E2E-05 "${selectors[@]}"; then
    local index actual
    for index in $(seq 1 100); do
      actual="$(curl --max-time 3 -sS \
        "$caller_url/send-node?targetRid=unknown-node&operationId=unknown-$index&sequence=$index")"
      [[ "$actual" == NOT_FOUND ]] || {
        echo "unknown target iteration=$index status=$actual" >&2
        return 1
      }
    done

    stop_pid "$target_pid"
    target_pid=""
    wait_route_disconnected "$caller_url" submit-target
    for index in $(seq 1 100); do
      actual="$(curl --max-time 3 -sS \
        "$caller_url/send-node?targetRid=submit-target&operationId=disconnected-$index&sequence=$index")"
      [[ "$actual" == UNAVAILABLE ]] || {
        echo "disconnected target iteration=$index status=$actual" >&2
        return 1
      }
    done
    record_pass SA-E2E-05 '"unknownCount":100,"unknown":"NOT_FOUND","disconnectedCount":100,"disconnected":"UNAVAILABLE"'
  fi

  local evidence
  for evidence in "$LOG_DIR"/*-evidence.jsonl; do
    [[ -f "$evidence" ]] && cat "$evidence" >>"$EVIDENCE_FILE"
  done
}

run_sa02() {
  local caller_http target_http caller_mesh target_mesh unused1 unused2 unused3
  read -r caller_http target_http caller_mesh target_mesh unused1 unused2 unused3 \
    < <(allocate_ports)
  local caller_url="http://127.0.0.1:$caller_http"
  local target_url="http://127.0.0.1:$target_http"
  local gate="$TEMP_DIR/sa02-gate.open"
  local payload
  payload="$(large_payload)"

  start_role sa02-target \
    --role=target --rid=submit-target --httpPort="$target_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_mesh" \
    --gateFile="$gate" --applicationHwmBytes=512 \
    --defaultRequestTimeoutMillis=5000 \
    --evidenceFile="$LOG_DIR/sa02-target-evidence.jsonl"
  start_role sa02-caller \
    --role=caller --rid=submit-caller --httpPort="$caller_http" \
    --meshEndpoint="tcp://127.0.0.1:$caller_mesh" \
    --peerRid=submit-target --peerEndpoint="tcp://127.0.0.1:$target_mesh" \
    --defaultRequestTimeoutMillis=5000 \
    --evidenceFile="$LOG_DIR/sa02-caller-evidence.jsonl"

  wait_health "$target_url" "${role_pids[sa02-target]}" sa02-target
  wait_health "$caller_url" "${role_pids[sa02-caller]}" sa02-caller
  wait_route_ready "$caller_url" submit-target
  expect_status Submitted \
    "$caller_url/send-node?targetRid=submit-target&operationId=handler-gate-blocker&sequence=1&payload=$payload"
  wait_handler_started "$target_url" handler-gate-blocker
  wait_runtime_paused "$target_url"
  curl --max-time 3 -fsS \
    "$caller_url/start-node?targetRid=submit-target&operationId=sa02-pending&sequence=2&payload=$payload" \
    >/dev/null
  assert_operation_pending "$caller_url" sa02-pending
  touch "$gate"
  wait_operation "$caller_url" sa02-pending Submitted
  wait_handler_counts "$target_url" sa02-pending started=1,completed=1
  record_pass SA-E2E-02 \
    '"applicationReceivePaused":true,"pendingTerminal":"Submitted","handlerCount":1,"resubmitCount":0'
}

run_sa03() {
  local caller_http target_a_http target_b_http caller_mesh target_a_mesh target_b_mesh unused
  read -r caller_http target_a_http target_b_http caller_mesh target_a_mesh target_b_mesh unused \
    < <(allocate_ports)
  local caller_url="http://127.0.0.1:$caller_http"
  local target_a_url="http://127.0.0.1:$target_a_http"
  local target_b_url="http://127.0.0.1:$target_b_http"
  local gate_a="$TEMP_DIR/sa03-a.open"
  local gate_b="$TEMP_DIR/sa03-b.open"
  local payload
  payload="$(large_payload)"

  start_role sa03-target-a \
    --role=target --rid=submit-target-a --httpPort="$target_a_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_a_mesh" \
    --gateFile="$gate_a" --applicationHwmBytes=512 \
    --defaultRequestTimeoutMillis=700 \
    --evidenceFile="$LOG_DIR/sa03-target-a-evidence.jsonl"
  start_role sa03-target-b \
    --role=target --rid=submit-target-b --httpPort="$target_b_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_b_mesh" \
    --gateFile="$gate_b" --applicationHwmBytes=512 \
    --defaultRequestTimeoutMillis=700 \
    --evidenceFile="$LOG_DIR/sa03-target-b-evidence.jsonl"
  start_role sa03-caller \
    --role=caller --rid=submit-caller --httpPort="$caller_http" \
    --meshEndpoint="tcp://127.0.0.1:$caller_mesh" \
    --peerRids=submit-target-a,submit-target-b \
    --peerEndpoints="tcp://127.0.0.1:$target_a_mesh,tcp://127.0.0.1:$target_b_mesh" \
    --defaultRequestTimeoutMillis=700 \
    --evidenceFile="$LOG_DIR/sa03-caller-evidence.jsonl"

  wait_health "$target_a_url" "${role_pids[sa03-target-a]}" sa03-target-a
  wait_health "$target_b_url" "${role_pids[sa03-target-b]}" sa03-target-b
  wait_health "$caller_url" "${role_pids[sa03-caller]}" sa03-caller
  wait_route_ready "$caller_url" submit-target-a
  wait_route_ready "$caller_url" submit-target-b
  expect_status Submitted \
    "$caller_url/send-node?targetRid=submit-target-a&operationId=handler-gate-a&sequence=1&payload=$payload"
  expect_status Submitted \
    "$caller_url/send-node?targetRid=submit-target-b&operationId=handler-gate-b&sequence=1&payload=$payload"
  wait_handler_started "$target_a_url" handler-gate-a
  wait_handler_started "$target_b_url" handler-gate-b
  wait_runtime_paused "$target_a_url"
  wait_runtime_paused "$target_b_url"
  curl --max-time 3 -fsS \
    "$caller_url/start-node?targetRid=submit-target-a&operationId=sa03-success&sequence=2&payload=$payload" \
    >/dev/null
  curl --max-time 3 -fsS \
    "$caller_url/start-node?targetRid=submit-target-b&operationId=sa03-deadline&sequence=2&payload=$payload" \
    >/dev/null
  assert_operation_pending "$caller_url" sa03-success
  assert_operation_pending "$caller_url" sa03-deadline
  touch "$gate_a"
  wait_operation "$caller_url" sa03-success Submitted
  wait_operation "$caller_url" sa03-deadline DEADLINE_EXCEEDED
  wait_handler_counts "$target_a_url" sa03-success started=1,completed=1
  wait_handler_counts "$target_b_url" sa03-deadline started=0,completed=0
  record_pass SA-E2E-03 \
    '"successTerminal":"Submitted","deadlineTerminal":"DEADLINE_EXCEEDED","successHandlerCount":1,"deadlineHandlerCount":0'
}

run_sa04() {
  local caller_http target_http caller_mesh target_mesh unused1 unused2 unused3
  read -r caller_http target_http caller_mesh target_mesh unused1 unused2 unused3 \
    < <(allocate_ports)
  local caller_url="http://127.0.0.1:$caller_http"
  local target_url="http://127.0.0.1:$target_http"
  local gate="$TEMP_DIR/sa04-gate.open"
  local payload
  payload="$(large_payload)"

  start_role sa04-target \
    --role=target --rid=submit-target --httpPort="$target_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_mesh" \
    --gateFile="$gate" --applicationHwmBytes=512 \
    --defaultRequestTimeoutMillis=600 \
    --evidenceFile="$LOG_DIR/sa04-target-evidence.jsonl"
  start_role sa04-caller \
    --role=caller --rid=submit-caller --httpPort="$caller_http" \
    --meshEndpoint="tcp://127.0.0.1:$caller_mesh" \
    --peerRid=submit-target --peerEndpoint="tcp://127.0.0.1:$target_mesh" \
    --defaultRequestTimeoutMillis=600 \
    --evidenceFile="$LOG_DIR/sa04-caller-evidence.jsonl"

  wait_health "$target_url" "${role_pids[sa04-target]}" sa04-target
  wait_health "$caller_url" "${role_pids[sa04-caller]}" sa04-caller
  wait_route_ready "$caller_url" submit-target
  expect_status Submitted \
    "$caller_url/send-node?targetRid=submit-target&operationId=handler-gate-blocker&sequence=1&payload=$payload"
  wait_handler_started "$target_url" handler-gate-blocker
  wait_runtime_paused "$target_url"
  curl --max-time 3 -fsS \
    "$caller_url/start-node?targetRid=submit-target&operationId=sa04-expired&sequence=2&payload=$payload" \
    >/dev/null
  assert_operation_pending "$caller_url" sa04-expired
  wait_operation "$caller_url" sa04-expired DEADLINE_EXCEEDED
  touch "$gate"
  sleep 0.5
  wait_handler_counts "$target_url" sa04-expired started=0,completed=0
  expect_status Submitted \
    "$caller_url/send-node?targetRid=submit-target&operationId=sa04-recovery&sequence=3"
  wait_handler_counts "$target_url" sa04-recovery started=1,completed=1
  record_pass SA-E2E-04 \
    '"expiredTerminal":"DEADLINE_EXCEEDED","expiredHandlerCount":0,"recoveryTerminal":"Submitted","recoveryHandlerCount":1'
}

run_sa07() {
  local caller_http target_http publisher_http subscriber_http caller_mesh target_mesh fanout_port unused
  read -r caller_http target_http publisher_http subscriber_http caller_mesh target_mesh fanout_port unused \
    < <(allocate_ports)
  local caller_url="http://127.0.0.1:$caller_http"
  local target_url="http://127.0.0.1:$target_http"
  local publisher_url="http://127.0.0.1:$publisher_http"
  local subscriber_url="http://127.0.0.1:$subscriber_http"
  local send_gate="$TEMP_DIR/sa07-send.open"
  local publish_gate="$TEMP_DIR/sa07-publish.open"
  local payload
  payload="$(large_payload)"

  start_role sa07-target \
    --role=target --rid=submit-target --httpPort="$target_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_mesh" \
    --gateFile="$send_gate" --applicationHwmBytes=512 \
    --defaultRequestTimeoutMillis=5000 \
    --evidenceFile="$LOG_DIR/sa07-target-evidence.jsonl"
  start_role sa07-caller \
    --role=caller --rid=submit-caller --httpPort="$caller_http" \
    --meshEndpoint="tcp://127.0.0.1:$caller_mesh" \
    --peerRid=submit-target --peerEndpoint="tcp://127.0.0.1:$target_mesh" \
    --defaultRequestTimeoutMillis=5000 \
    --evidenceFile="$LOG_DIR/sa07-caller-evidence.jsonl"
  start_role sa07-publisher \
    --role=publisher --rid=submit-publisher --httpPort="$publisher_http" \
    --fanoutEndpoint="tcp://127.0.0.1:$fanout_port" \
    --evidenceFile="$LOG_DIR/sa07-publisher-evidence.jsonl"
  start_role sa07-subscriber \
    --role=subscriber --rid=submit-subscriber --httpPort="$subscriber_http" \
    --fanoutEndpoint="tcp://127.0.0.1:$fanout_port" \
    --gateFile="$publish_gate" \
    --evidenceFile="$LOG_DIR/sa07-subscriber-evidence.jsonl"

  wait_health "$target_url" "${role_pids[sa07-target]}" sa07-target
  wait_health "$caller_url" "${role_pids[sa07-caller]}" sa07-caller
  wait_health "$publisher_url" "${role_pids[sa07-publisher]}" sa07-publisher
  wait_health "$subscriber_url" "${role_pids[sa07-subscriber]}" sa07-subscriber
  wait_route_ready "$caller_url" submit-target
  expect_status Submitted \
    "$caller_url/send-node?targetRid=submit-target&operationId=handler-gate-blocker&sequence=1&payload=$payload"
  wait_handler_started "$target_url" handler-gate-blocker
  wait_runtime_paused "$target_url"
  curl --max-time 3 -fsS \
    "$caller_url/start-node?targetRid=submit-target&operationId=sa07-cancelled&sequence=2&payload=$payload" \
    >/dev/null
  assert_operation_pending "$caller_url" sa07-cancelled
  expect_body CANCEL_REQUESTED "$caller_url/cancel?operationId=sa07-cancelled"
  wait_operation "$caller_url" sa07-cancelled CANCELLED
  touch "$send_gate"
  wait_handler_counts "$target_url" sa07-cancelled started=0,completed=0

  sleep 0.5
  curl --max-time 3 -fsS \
    "$publisher_url/start-publish?operationId=sa07-publish-commit&sequence=3" >/dev/null
  wait_operation "$publisher_url" sa07-publish-commit Submitted
  wait_handler_started "$subscriber_url" sa07-publish-commit
  expect_body NOT_PENDING "$publisher_url/cancel?operationId=sa07-publish-commit"
  touch "$publish_gate"
  wait_handler_counts "$subscriber_url" sa07-publish-commit started=1,completed=1
  record_pass SA-E2E-07 \
    '"preCommitTerminal":"CANCELLED","cancelledHandlerCount":0,"publishTerminal":"Submitted","publishHandlerCount":1'
}

run_clientserver_variant() {
  local variant="$1" expected="$2"
  local caller_http target_http caller_mesh target_mesh cs_port unused1 unused2
  read -r caller_http target_http caller_mesh target_mesh cs_port unused1 unused2 \
    < <(allocate_ports)
  local caller_url="http://127.0.0.1:$caller_http"
  local target_url="http://127.0.0.1:$target_http"
  local gate="$TEMP_DIR/sa10-$variant.open"
  local payload
  payload="$(large_payload)"

  start_role "sa10-$variant-server" \
    --role=target --rid=submit-cs-$variant --httpPort="$target_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_mesh" \
    --clientServerRole=server --clientServerPort="$cs_port" \
    --gateFile="$gate" --applicationHwmBytes=512 \
    --defaultRequestTimeoutMillis=700 \
    --evidenceFile="$LOG_DIR/sa10-$variant-server-evidence.jsonl"
  start_role "sa10-$variant-client" \
    --role=caller --rid=submit-cs-caller-$variant --httpPort="$caller_http" \
    --meshEndpoint="tcp://127.0.0.1:$caller_mesh" \
    --clientServerRole=client \
    --clientServerEndpoint="tcp://127.0.0.1:$cs_port" \
    --defaultRequestTimeoutMillis=700 \
    --evidenceFile="$LOG_DIR/sa10-$variant-client-evidence.jsonl"

  wait_health "$target_url" "${role_pids[sa10-$variant-server]}" "sa10-$variant-server"
  wait_health "$caller_url" "${role_pids[sa10-$variant-client]}" "sa10-$variant-client"
  wait_client_server_ready "$caller_url"
  expect_status Submitted \
    "$caller_url/send-clientserver?operationId=handler-gate-blocker-$variant&sequence=1&payload=$payload"
  wait_handler_started "$target_url" "handler-gate-blocker-$variant"
  wait_runtime_paused "$target_url"
  curl --max-time 3 -fsS \
    "$caller_url/start-clientserver?operationId=sa10-$variant-operation&sequence=2&payload=$payload" \
    >/dev/null
  assert_operation_pending "$caller_url" "sa10-$variant-operation"
  if [[ "$expected" == Submitted ]]; then
    touch "$gate"
    wait_operation "$caller_url" "sa10-$variant-operation" Submitted
    wait_handler_counts "$target_url" "sa10-$variant-operation" started=1,completed=1
  else
    wait_operation "$caller_url" "sa10-$variant-operation" DEADLINE_EXCEEDED
    wait_handler_counts "$target_url" "sa10-$variant-operation" started=0,completed=0
  fi
  stop_role "sa10-$variant-server"
  stop_role "sa10-$variant-client"
}

run_sa10() {
  run_clientserver_variant success Submitted
  run_clientserver_variant timeout DEADLINE_EXCEEDED
  record_pass SA-E2E-10 \
    '"successTerminal":"Submitted","successHandlerCount":1,"timeoutTerminal":"DEADLINE_EXCEEDED","timeoutHandlerCount":0'
}

run_sa18() {
  local caller_http target_a_http target_b_http caller_mesh target_a_mesh target_b_mesh unused
  read -r caller_http target_a_http target_b_http caller_mesh target_a_mesh target_b_mesh unused \
    < <(allocate_ports)
  local caller_url="http://127.0.0.1:$caller_http"
  local target_a_url="http://127.0.0.1:$target_a_http"
  local target_b_url="http://127.0.0.1:$target_b_http"

  start_role sa18-target-a \
    --role=target --rid=submit-target-a --httpPort="$target_a_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_a_mesh" \
    --evidenceFile="$LOG_DIR/sa18-target-a-evidence.jsonl"
  start_role sa18-target-b \
    --role=target --rid=submit-target-b --httpPort="$target_b_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_b_mesh" \
    --evidenceFile="$LOG_DIR/sa18-target-b-evidence.jsonl"
  start_role sa18-caller \
    --role=caller --rid=submit-caller --httpPort="$caller_http" \
    --meshEndpoint="tcp://127.0.0.1:$caller_mesh" \
    --peerRids=submit-target-a,submit-target-b \
    --peerEndpoints="tcp://127.0.0.1:$target_a_mesh,tcp://127.0.0.1:$target_b_mesh" \
    --evidenceFile="$LOG_DIR/sa18-caller-evidence.jsonl"

  wait_health "$target_a_url" "${role_pids[sa18-target-a]}" sa18-target-a
  wait_health "$target_b_url" "${role_pids[sa18-target-b]}" sa18-target-b
  wait_health "$caller_url" "${role_pids[sa18-caller]}" sa18-caller
  wait_route_ready "$caller_url" submit-target-a
  wait_route_ready "$caller_url" submit-target-b
  stop_role sa18-target-a
  wait_route_disconnected "$caller_url" submit-target-a
  expect_body UNAVAILABLE \
    "$caller_url/send-node?targetRid=submit-target-a&operationId=sa18-direct-a&sequence=1"
  expect_status Submitted \
    "$caller_url/send-channel?operationId=sa18-channel-b&sequence=2"
  wait_handler_counts "$target_b_url" sa18-channel-b started=1,completed=1
  record_pass SA-E2E-18 \
    '"directUnavailable":"UNAVAILABLE","directTargetHandlerCount":0,"channelTerminal":"Submitted","selectedTarget":"submit-target-b","selectedTargetHandlerCount":1'
}

run_sa19() {
  local caller_http target_a_http target_b_http caller_mesh target_a_mesh target_b_mesh unused
  read -r caller_http target_a_http target_b_http caller_mesh target_a_mesh target_b_mesh unused \
    < <(allocate_ports)
  local caller_url="http://127.0.0.1:$caller_http"
  local target_a_url="http://127.0.0.1:$target_a_http"
  local target_b_url="http://127.0.0.1:$target_b_http"
  local old_gate="$TEMP_DIR/sa19-old-gate.open"

  start_role sa19-target-a \
    --role=target --rid=submit-target-a --httpPort="$target_a_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_a_mesh" \
    --gateFile="$old_gate" \
    --evidenceFile="$LOG_DIR/sa19-target-a-before-recovery-evidence.jsonl"
  start_role sa19-target-b \
    --role=target --rid=submit-target-b --httpPort="$target_b_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_b_mesh" \
    --evidenceFile="$LOG_DIR/sa19-target-b-evidence.jsonl"
  start_role sa19-caller \
    --role=caller --rid=submit-caller --httpPort="$caller_http" \
    --meshEndpoint="tcp://127.0.0.1:$caller_mesh" \
    --peerRids=submit-target-a,submit-target-b \
    --peerEndpoints="tcp://127.0.0.1:$target_a_mesh,tcp://127.0.0.1:$target_b_mesh" \
    --evidenceFile="$LOG_DIR/sa19-caller-evidence.jsonl"

  wait_health "$target_a_url" "${role_pids[sa19-target-a]}" sa19-target-a
  wait_health "$target_b_url" "${role_pids[sa19-target-b]}" sa19-target-b
  wait_health "$caller_url" "${role_pids[sa19-caller]}" sa19-caller
  wait_route_ready "$caller_url" submit-target-a
  wait_route_ready "$caller_url" submit-target-b
  stop_role sa19-target-a
  wait_route_disconnected "$caller_url" submit-target-a
  expect_body UNAVAILABLE \
    "$caller_url/send-node?targetRid=submit-target-a&operationId=sa19-terminal&sequence=1"
  wait_handler_counts "$target_b_url" sa19-terminal started=0,completed=0

  start_role sa19-target-a-recovered \
    --role=target --rid=submit-target-a --httpPort="$target_a_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_a_mesh" \
    --evidenceFile="$LOG_DIR/sa19-target-a-after-recovery-evidence.jsonl"
  wait_health "$target_a_url" "${role_pids[sa19-target-a-recovered]}" sa19-target-a-recovered
  wait_route_ready "$caller_url" submit-target-a
  sleep 0.5
  wait_handler_counts "$target_a_url" sa19-terminal started=0,completed=0
  expect_status Submitted \
    "$caller_url/send-node?targetRid=submit-target-a&operationId=sa19-recovery&sequence=2"
  wait_handler_counts "$target_a_url" sa19-recovery started=1,completed=1
  record_pass SA-E2E-19 \
    '"terminal":"UNAVAILABLE","terminalHandlerCount":0,"recoveredTerminal":"Submitted","recoveredHandlerCount":1,"lateReplayCount":0'
}

run_sa06_shutdown() {
  local caller_http target_http caller_mesh target_mesh unused1 unused2 unused3
  read -r caller_http target_http caller_mesh target_mesh unused1 unused2 unused3 \
    < <(allocate_ports)
  local caller_url="http://127.0.0.1:$caller_http"
  local target_url="http://127.0.0.1:$target_http"

  start_role sa06-shutdown-target \
    --role=target --rid=submit-target --httpPort="$target_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_mesh" \
    --evidenceFile="$LOG_DIR/sa06-shutdown-target-evidence.jsonl"
  start_role sa06-shutdown-caller \
    --role=caller --rid=submit-caller --httpPort="$caller_http" \
    --meshEndpoint="tcp://127.0.0.1:$caller_mesh" \
    --peerRid=submit-target --peerEndpoint="tcp://127.0.0.1:$target_mesh" \
    --evidenceFile="$LOG_DIR/sa06-shutdown-caller-evidence.jsonl"

  wait_health "$target_url" "${role_pids[sa06-shutdown-target]}" sa06-shutdown-target
  wait_health "$caller_url" "${role_pids[sa06-shutdown-caller]}" sa06-shutdown-caller
  wait_route_ready "$caller_url" submit-target
  expect_body SHUTDOWN_REQUESTED "$caller_url/shutdown"
  wait_runtime_not_accepting "$caller_url"
  local actual
  actual="$(curl --max-time 3 -sS \
    "$caller_url/send-node?targetRid=submit-target&operationId=sa06-after-shutdown&sequence=1" \
    || true)"
  wait_handler_counts "$target_url" sa06-after-shutdown started=0,completed=0
  if [[ "$actual" == SHUTTING_DOWN ]]; then
    echo "SA-E2E-06 Shutdown variant PASS terminal=$actual"
    echo "Relocate variant remains blocked: no public Location/Relocation Store fixture" >&2
    return 3
  fi
  echo "SA-E2E-06 Shutdown variant blocked terminal=$actual" >&2
  echo "Relocate variant remains blocked: no public Location/Relocation Store fixture" >&2
  return 3
}

selectors=()
if [[ "$SCENARIO" == all ]]; then
  echo "SubmitAdmission Kotlin aggregate is incomplete; blocked selectors:" >&2
  for selector in "${blocked[@]}"; do
    echo "  $selector: ${blocker_reason[$selector]}" >&2
  done
  exit 3
elif [[ "$SCENARIO" == supported ]]; then
  selectors=("${supported[@]}")
else
  IFS=',' read -r -a selectors <<<"$SCENARIO"
fi

for selector in "${selectors[@]}"; do
  if ! is_known "$selector"; then
    echo "unknown SubmitAdmission selector: $selector" >&2
    exit 2
  fi
  if contains "$selector" "${blocked[@]}"; then
    echo "$selector is blocked: ${blocker_reason[$selector]}" >&2
    echo "see $SCRIPT_DIR/feature-map.ko.md" >&2
    exit 3
  fi
done

build_role > >(tee "$LOG_DIR/build.log") 2>&1

legacy_selectors=()
for selector in "${selectors[@]}"; do
  case "$selector" in
    SA-E2E-01|SA-E2E-05|SA-E2E-08|SA-E2E-09|SA-E2E-14|SA-E2E-20)
      legacy_selectors+=("$selector")
      ;;
  esac
done

if ((${#legacy_selectors[@]} > 0)); then
  run_process_scenarios "${legacy_selectors[@]}" 2>&1 | tee "$LOG_DIR/process.log"
fi
if contains SA-E2E-02 "${selectors[@]}"; then
  run_sa02 2>&1 | tee -a "$LOG_DIR/process.log"
fi
if contains SA-E2E-03 "${selectors[@]}"; then
  run_sa03 2>&1 | tee -a "$LOG_DIR/process.log"
fi
if contains SA-E2E-04 "${selectors[@]}"; then
  run_sa04 2>&1 | tee -a "$LOG_DIR/process.log"
fi
if contains SA-E2E-07 "${selectors[@]}"; then
  run_sa07 2>&1 | tee -a "$LOG_DIR/process.log"
fi
if contains SA-E2E-10 "${selectors[@]}"; then
  run_sa10 2>&1 | tee -a "$LOG_DIR/process.log"
fi
if contains SA-E2E-18 "${selectors[@]}"; then
  run_sa18 2>&1 | tee -a "$LOG_DIR/process.log"
fi
if contains SA-E2E-19 "${selectors[@]}"; then
  run_sa19 2>&1 | tee -a "$LOG_DIR/process.log"
fi
if contains SA-E2E-06-shutdown "${selectors[@]}"; then
  run_sa06_shutdown 2>&1 | tee -a "$LOG_DIR/process.log"
fi

echo "SubmitAdmission Kotlin PASS scenarios=${selectors[*]} logs=$LOG_DIR"
