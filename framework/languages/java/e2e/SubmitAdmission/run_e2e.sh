#!/usr/bin/env bash
set -euo pipefail
umask 077

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../../../.." && pwd)"
JAVA_ROOT="$REPO_ROOT/framework/languages/java"
ROLE_ROOT="$SCRIPT_DIR/Role"
SCENARIO="${1:-all}"
STAMP="$(date +%Y%m%d-%H%M%S)-$$"
LOG_DIR="$SCRIPT_DIR/logs/$STAMP"
TEMP_DIR="$(mktemp -d)"
EVIDENCE_FILE="$LOG_DIR/evidence.jsonl"
mkdir -p "$LOG_DIR"

BINDING_VERSION="$(awk -F'"' '$1 ~ /^zlinkBindings[[:space:]]*=/ { print $2; exit }' \
  "$JAVA_ROOT/gradle/libs.versions.toml")"
CORE_PACKAGE_PREFIX=""
CORE_PACKAGE_EVIDENCE=""
CANDIDATE_CORE_VERSION=""
CANDIDATE_RUNTIME_SHA256=""

implemented_process=(SA-E2E-01 SA-E2E-05 SA-E2E-08 SA-E2E-09 SA-E2E-14 SA-E2E-20)
implemented_regression=(SA-REG-01 SA-REG-02 SA-REG-03)
implemented=("${implemented_process[@]}" "${implemented_regression[@]}")
CANDIDATE_JAR=""
CANDIDATE_SHA256=""
pids=()
TARGET_PID=""

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
  [[ "$1" =~ ^SA-E2E-(0[1-9]|1[0-9]|20)$ || "$1" =~ ^SA-REG-0[1-4]$ ]]
}

resolve_candidate() {
  if [[ -z "${ZLINK_LOCAL_PACKAGE_ROOT:-}" ]]; then
    echo "ZLINK_LOCAL_PACKAGE_ROOT must identify the isolated JVM binding candidate" >&2
    return 1
  fi
  if [[ -z "${ZLINK_CORE_PACKAGE_PREFIX:-}" || -z "${ZLINK_CORE_PACKAGE_EVIDENCE:-}" ]]; then
    echo "ZLINK_CORE_PACKAGE_PREFIX and ZLINK_CORE_PACKAGE_EVIDENCE must identify the approved Core package" >&2
    return 1
  fi
  [[ -d "$ZLINK_CORE_PACKAGE_PREFIX" ]] || {
    echo "approved Core package prefix does not exist: $ZLINK_CORE_PACKAGE_PREFIX" >&2
    return 1
  }
  [[ -f "$ZLINK_CORE_PACKAGE_EVIDENCE" ]] || {
    echo "approved Core package evidence does not exist: $ZLINK_CORE_PACKAGE_EVIDENCE" >&2
    return 1
  }
  CORE_PACKAGE_PREFIX="$(realpath "$ZLINK_CORE_PACKAGE_PREFIX")"
  CORE_PACKAGE_EVIDENCE="$(realpath "$ZLINK_CORE_PACKAGE_EVIDENCE")"
  node "$REPO_ROOT/scripts/local-package/java/verify-core-input.mjs" \
    --prefix "$CORE_PACKAGE_PREFIX" \
    --core-package-evidence "$CORE_PACKAGE_EVIDENCE" \
    >"$TEMP_DIR/core-package-summary.json"

  CANDIDATE_JAR="$(find "$ZLINK_LOCAL_PACKAGE_ROOT/maven/systems/zlink/zlink" \
    -path "*/$BINDING_VERSION/zlink-$BINDING_VERSION.jar" -type f -print -quit)"
  if [[ -z "$CANDIDATE_JAR" ]]; then
    echo "zlink Java binding candidate $BINDING_VERSION was not found" >&2
    return 1
  fi
  CANDIDATE_JAR="$(realpath "$CANDIDATE_JAR")"
  CANDIDATE_SHA256="$(sha256sum "$CANDIDATE_JAR" | awk '{print $1}')"

  local provenance="$TEMP_DIR/candidate-core-package-provenance.json"
  unzip -p "$CANDIDATE_JAR" META-INF/zlink/core-package-provenance.json >"$provenance"
  read -r CANDIDATE_CORE_VERSION CANDIDATE_RUNTIME_SHA256 < <(
    node - "$provenance" <<'NODE'
const fs = require('node:fs');
const provenance = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const runtime = provenance.files?.find(record =>
  record.path === `lib/libzlink.so.${provenance.version}`);
if (!runtime) throw new Error('candidate Core provenance has no versioned runtime record');
process.stdout.write(`${provenance.version} ${runtime.sha256}\n`);
NODE
  )
}

print_candidate() {
  echo "binding_candidate=$CANDIDATE_JAR"
  echo "$CANDIDATE_SHA256  $CANDIDATE_JAR"
  local core_runtime="$CORE_PACKAGE_PREFIX/lib/libzlink.so"
  local native_entry="native/linux-x86_64/libzlink.so"
  local core_sha packaged_sha native_copy core_package_version
  core_package_version="$(node -e \
    'process.stdout.write(JSON.parse(require("fs").readFileSync(process.argv[1])).version)' \
    "$TEMP_DIR/core-package-summary.json")"
  [[ "$CANDIDATE_CORE_VERSION" == "$core_package_version" ]] || {
    echo "candidate Core version does not match approved Core package" >&2
    echo "candidate=$CANDIDATE_CORE_VERSION approved=$core_package_version" >&2
    return 1
  }
  core_sha="$(sha256sum "$core_runtime" | awk '{print $1}')"
  packaged_sha="$(unzip -p "$CANDIDATE_JAR" "$native_entry" | sha256sum | awk '{print $1}')"
  if [[ "$packaged_sha" != "$core_sha" || "$packaged_sha" != "$CANDIDATE_RUNTIME_SHA256" ]]; then
    echo "candidate native runtime does not match approved Core package" >&2
    echo "core_sha=$core_sha packaged_sha=$packaged_sha" >&2
    return 1
  fi
  native_copy="$TEMP_DIR/candidate-libzlink.so"
  unzip -p "$CANDIDATE_JAR" "$native_entry" >"$native_copy"
  echo "core_runtime_sha256=$core_sha"
  readelf -n "$native_copy" | rg 'Build ID'
}

run_gradle_with_candidate() {
  local project="$1"
  shift
  (
    cd "$JAVA_ROOT"
    env -u ZLINK_JAVA_BINDINGS_SOURCE \
      ZLINK_LOCAL_PACKAGE_ROOT="$ZLINK_LOCAL_PACKAGE_ROOT" \
      ZLINK_EXPECTED_BINDING_JAR="$CANDIDATE_JAR" \
      ZLINK_EXPECTED_BINDING_SHA256="$CANDIDATE_SHA256" \
      ZLINK_EXPECTED_BINDING_VERSION="$BINDING_VERSION" \
      ./gradlew \
        -Pzlink.localPackageRoot="$ZLINK_LOCAL_PACKAGE_ROOT" \
        --refresh-dependencies \
        --init-script "$SCRIPT_DIR/verify-candidate-classpath.init.gradle" \
        "$project:verifyZlinkCandidateClasspath" \
        "$@"
  )
}

build_role() {
  (
    cd "$ROLE_ROOT"
    env -u ZLINK_JAVA_BINDINGS_SOURCE \
      ZLINK_LOCAL_PACKAGE_ROOT="$ZLINK_LOCAL_PACKAGE_ROOT" \
      ZLINK_EXPECTED_BINDING_JAR="$CANDIDATE_JAR" \
      ZLINK_EXPECTED_BINDING_SHA256="$CANDIDATE_SHA256" \
      ZLINK_EXPECTED_BINDING_VERSION="$BINDING_VERSION" \
      ../../../gradlew \
        -Pzlink.localPackageRoot="$ZLINK_LOCAL_PACKAGE_ROOT" \
        --refresh-dependencies \
        --init-script "$SCRIPT_DIR/verify-candidate-classpath.init.gradle" \
        verifyZlinkCandidateClasspath installDist
  )
}

run_reg_01() {
  bash "$REPO_ROOT/scripts/verify-framework-submit-api.sh" --contract
  bash "$REPO_ROOT/scripts/verify-framework-submit-api.sh" --implementation
  echo '{"scenarioId":"SA-REG-01","status":"PASS","publicTrySubmitHits":0}' \
    >>"$EVIDENCE_FILE"
}

run_reg_02() {
  run_gradle_with_candidate :zlink-framework-core :zlink-framework-core:test \
    --tests systems.zlink.framework.runtime.messaging.ZLinkAdmissionRuntimeTest \
    --tests systems.zlink.framework.runtime.ZLinkAsyncSubmitterTest \
    --tests systems.zlink.framework.runtime.mesh.MeshNodeRegistrationSubmitTimeoutTest \
    --tests systems.zlink.framework.runtime.mesh.ZLinkMeshNodeRuntimeTest \
    --tests systems.zlink.framework.execution.ZLinkAsyncSerialQueueTest \
    --tests systems.zlink.framework.runtime.streams.ZLinkStreamSessionContextStateTest \
    --tests systems.zlink.framework.runtime.actors.ZLinkActorClientRuntimeTest \
    --tests systems.zlink.framework.runtime.channels.ZLinkChannelRuntimeTest.localNodeSendWaitsForTheExactCapacitySignalWithoutPublicRetry \
    --tests systems.zlink.framework.runtime.channels.ZLinkChannelRuntimeTest.localNodeTimeoutPreventsLateAdmission \
    --tests systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcherTest.localNodeSendUsesBoundedPendingCapacityAndEmitsReadyOnce \
    --tests systems.zlink.framework.runtime.host.NodesAndServicesTest.routeMeshDispatchesSpotRequestToTargetSpot \
    --tests systems.zlink.framework.runtime.channels.ZLinkMeshApplicationDispatcherTest
  echo '{"scenarioId":"SA-REG-02","status":"PASS","localNodeBridge":"focused"}' \
    >>"$EVIDENCE_FILE"
}

run_reg_03() {
  run_gradle_with_candidate :zlink-framework-kotlin :zlink-framework-kotlin:test \
    --tests systems.zlink.framework.kotlin.KotlinFrameworkExtensionsContractTest
  echo '{"scenarioId":"SA-REG-03","status":"PASS","projection":"cancel(false)"}' \
    >>"$EVIDENCE_FILE"
}

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
  local pid
  for pid in "${pids[@]}"; do
    stop_pid "$pid"
  done
  rm -rf "$TEMP_DIR"
  if [[ "$code" -ne 0 ]]; then
    local log
    for log in "$LOG_DIR"/*.stderr.log; do
      [[ -f "$log" ]] || continue
      echo "==> $log" >&2
      tail -80 "$log" >&2 || true
    done
  fi
}
trap cleanup EXIT

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
  for _ in $(seq 1 30); do
    if curl --max-time 1 -fsS "$url/ready?targetRid=$target_rid" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "target route did not become ready within 3 seconds" >&2
  return 1
}

wait_route_disconnected() {
  local url="$1" target_rid="$2"
  # RouteMesh liveness uses a 5 s probe interval and a 15 s peer deadline.
  # Allow one complete deadline window after an abrupt process stop.
  for _ in $(seq 1 200); do
    if ! curl --max-time 1 -fsS "$url/ready?targetRid=$target_rid" >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "target route remained ready after target process stopped for 20 seconds" >&2
  return 1
}

expect_status() {
  local expected="$1" url="$2"
  local actual
  actual="$(curl --max-time 3 -fsS "$url")"
  if [[ "$actual" != "$expected" ]]; then
    echo "expected status=$expected actual=$actual url=$url" >&2
    return 1
  fi
}

wait_counts() {
  local url="$1" expected_started="$2" expected_completed="$3"
  local actual=""
  for _ in $(seq 1 30); do
    actual="$(curl --max-time 1 -fsS "$url/counts")"
    if [[ "$actual" == "started=$expected_started,completed=$expected_completed" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected counts=started=$expected_started,completed=$expected_completed actual=$actual" >&2
  return 1
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
  local role_bin="$ROLE_ROOT/build/install/submit-admission-role/bin/submit-admission-role"

  "$role_bin" \
    --role=target --rid=submit-target --httpPort="$target_http" \
    --meshEndpoint="tcp://127.0.0.1:$target_mesh" \
    --gateFile="$gate" --evidenceFile="$LOG_DIR/target-evidence.jsonl" \
    >"$LOG_DIR/target.stdout.log" 2>"$LOG_DIR/target.stderr.log" &
  TARGET_PID=$!
  pids+=("$TARGET_PID")

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

  wait_health "$target_url" "$TARGET_PID" target
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
    echo '{"scenarioId":"SA-E2E-01","status":"PASS","publicSubmitCount":2,"terminal":"Submitted"}' \
      >>"$EVIDENCE_FILE"
  fi

  if contains SA-E2E-08 "${selectors[@]}"; then
    expect_status Submitted \
      "$caller_url/send-node?targetRid=submit-caller&operationId=local-direct&sequence=3"
    expect_status Submitted \
      "$caller_url/send-node?targetRid=submit-target&operationId=remote-direct&sequence=4"
    caller_expected=$((caller_expected + 1))
    target_expected=$((target_expected + 1))
    echo '{"scenarioId":"SA-E2E-08","status":"PASS","local":"Submitted","remote":"Submitted","handlerCountEach":1}' \
      >>"$EVIDENCE_FILE"
  fi

  if contains SA-E2E-09 "${selectors[@]}"; then
    expect_status Submitted \
      "$caller_url/send-channel?operationId=positive-weight-channel&sequence=5"
    target_expected=$((target_expected + 1))
    echo '{"scenarioId":"SA-E2E-09","status":"PASS","positiveWeightHandler":1,"terminal":"Submitted"}' \
      >>"$EVIDENCE_FILE"
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
    echo '{"scenarioId":"SA-E2E-14","status":"PASS","subscriberProcessCountAtSubmit":0,"terminal":"Submitted","lateSubscriberDeliveryCount":0}' \
      >>"$EVIDENCE_FILE"
  fi

  if contains SA-E2E-20 "${selectors[@]}"; then
    expect_status Submitted \
      "$caller_url/send-node?targetRid=submit-target&operationId=handler-gate-remote&sequence=7"
    target_expected=$((target_expected + 1))
    wait_counts "$target_url" "$target_expected" "$((target_expected - 1))"
    touch "$gate"
    wait_counts "$target_url" "$target_expected" "$target_expected"
    echo '{"scenarioId":"SA-E2E-20","status":"PASS","submitBeforeHandlerCompletion":true,"handlerCount":1}' \
      >>"$EVIDENCE_FILE"
  fi

  wait_counts "$caller_url" "$caller_expected" "$caller_expected"
  if ! contains SA-E2E-20 "${selectors[@]}"; then
    wait_counts "$target_url" "$target_expected" "$target_expected"
  fi

  if contains SA-E2E-05 "${selectors[@]}"; then
    local index actual
    for index in $(seq 1 100); do
      actual="$(curl --max-time 3 -sS \
        "$caller_url/send-node?targetRid=unknown-node&operationId=unknown-$index&sequence=$index")"
      [[ "$actual" == REQUEST_TARGET_NOT_FOUND ]] || {
        echo "unknown target iteration=$index status=$actual" >&2
        return 1
      }
    done

    stop_pid "$TARGET_PID"
    TARGET_PID=""
    wait_route_disconnected "$caller_url" submit-target
    for index in $(seq 1 100); do
      actual="$(curl --max-time 3 -sS \
        "$caller_url/send-node?targetRid=submit-target&operationId=disconnected-$index&sequence=$index")"
      [[ "$actual" == ROUTE_NOT_CONNECTED ]] || {
        echo "disconnected target iteration=$index status=$actual" >&2
        return 1
      }
    done
    echo '{"scenarioId":"SA-E2E-05","status":"PASS","unknownCount":100,"unknown":"REQUEST_TARGET_NOT_FOUND","disconnectedCount":100,"disconnected":"ROUTE_NOT_CONNECTED"}' \
      >>"$EVIDENCE_FILE"
  fi

  for evidence in "$LOG_DIR"/*-evidence.jsonl; do
    [[ -f "$evidence" ]] && cat "$evidence" >>"$EVIDENCE_FILE"
  done
}

resolve_candidate
print_candidate | tee "$LOG_DIR/candidate.log"

selectors=()
if [[ "$SCENARIO" == all ]]; then
  selectors=("${implemented[@]}")
else
  IFS=',' read -r -a selectors <<<"$SCENARIO"
fi

for selector in "${selectors[@]}"; do
  if ! is_known "$selector"; then
    echo "unknown SubmitAdmission selector: $selector" >&2
    exit 2
  fi
  if ! contains "$selector" "${implemented[@]}"; then
    echo "$selector is not implemented; see $SCRIPT_DIR/feature-map.ko.md" >&2
    exit 3
  fi
done

process_selectors=()
for selector in "${selectors[@]}"; do
  case "$selector" in
    SA-REG-01) run_reg_01 2>&1 | tee "$LOG_DIR/SA-REG-01.log" ;;
    SA-REG-02) run_reg_02 2>&1 | tee "$LOG_DIR/SA-REG-02.log" ;;
    SA-REG-03) run_reg_03 2>&1 | tee "$LOG_DIR/SA-REG-03.log" ;;
    *) process_selectors+=("$selector") ;;
  esac
done

if [[ "${#process_selectors[@]}" -gt 0 ]]; then
  build_role 2>&1 | tee "$LOG_DIR/package-mode.log"
  run_process_scenarios "${process_selectors[@]}" 2>&1 | tee "$LOG_DIR/process.log"
fi

echo "SubmitAdmission JVM PASS scenarios=${selectors[*]} logs=$LOG_DIR"
