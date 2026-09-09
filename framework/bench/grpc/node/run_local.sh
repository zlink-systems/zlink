#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../../.." && pwd)"
cd "${HERE}"

export NODE_PATH="${REPO}/framework/languages/node/node_modules${NODE_PATH:+:${NODE_PATH}}"

RUNS="${RUNS:-1}"
RUN_DEALER="${RUN_DEALER:-0}"
DURATION="${DURATION:-5}"
WARMUP="${WARMUP:-1000}"
PAYLOADS="${PAYLOADS:-1024,4096}"
SCENARIO="${SCENARIO:-all}"
IMPLEMENTATION="${IMPLEMENTATION:-all}"
WINDOW="${WINDOW:-100}"
STAMP="${STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUTROOT="${OUTROOT:-${HERE}/../log/node/with_grpc_node_${STAMP}}"
SEND_CONCURRENCY=8
TIMEOUT_SECONDS=300
COMMAND_SETTLE_MS=200
DRAIN_BOUND_MS=30000
REQUEST_TIMEOUT_MS=30000
ROUTE_READY_MS=30000
LATENCY_SAMPLE_LIMIT=200000
UNSUPPORTED_REASON='framework-codec-protobuf has no bytes value kind; protobuf bytes body is encoded as an object and does not round-trip as bytes'

[[ "${RUNS}" =~ ^[1-9][0-9]*$ ]] || { echo "RUNS must be a positive integer" >&2; exit 2; }
[[ "${RUN_DEALER}" == "0" ]] || { echo "RUN_DEALER is unsupported; raw comparison is ROUTER<->ROUTER" >&2; exit 2; }
[[ "${DURATION}" =~ ^[1-9][0-9]*$ ]] || { echo "DURATION must be a positive integer" >&2; exit 2; }
[[ "${WARMUP}" =~ ^[0-9]+$ ]] || { echo "WARMUP must be a non-negative integer" >&2; exit 2; }
[[ "${WINDOW}" == "100" ]] || { echo "WINDOW must remain 100" >&2; exit 2; }

IFS=',' read -r -a payloads <<<"${PAYLOADS}"
for payload in "${payloads[@]}"; do
  [[ "${payload}" == "1024" || "${payload}" == "4096" ]] || {
    echo "PAYLOADS entries must be 1024 or 4096" >&2
    exit 2
  }
done

case "${SCENARIO}" in
  all) patterns=(request-serial request-window request-backpressure send-saturation) ;;
  request) patterns=(request-serial request-window request-backpressure) ;;
  request-serial|request-window|request-backpressure|send-saturation) patterns=("${SCENARIO}") ;;
  send|command) patterns=(send-saturation) ;;
  *) echo "unknown SCENARIO: ${SCENARIO}" >&2; exit 2 ;;
esac

case "${IMPLEMENTATION}" in
  all) implementations=(grpc-node zlink-node zlink-framework-node) ;;
  grpc-node|zlink-node|zlink-framework-node) implementations=("${IMPLEMENTATION}") ;;
  *) echo "unknown IMPLEMENTATION: ${IMPLEMENTATION}" >&2; exit 2 ;;
esac

check_ports_free() {
  local used
  used="$(ss -H -ltn | awk '
    {
      endpoint=$4
      sub(/^.*:/, "", endpoint)
      if (endpoint ~ /^[0-9]+$/ && endpoint >= 5220 && endpoint <= 5239) print $4
    }')"
  if [[ -n "${used}" ]]; then
    echo "node bench port range 5220-5239 has active listeners: ${used}" >&2
    return 1
  fi
}

wait_for_ports_free() {
  local deadline=$((SECONDS + 30))
  while ((SECONDS < deadline)); do
    if check_ports_free 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  check_ports_free
}

wait_for_stats() {
  local url="$1" require_ready="$2"
  local deadline=$((SECONDS + 30))
  while ((SECONDS < deadline)); do
    local body
    if body="$(curl --silent --show-error --fail "${url}/bench/stats" 2>/dev/null)"; then
      if [[ "${require_ready}" != "1" ]] || python3 -c \
        'import json,sys; raise SystemExit(0 if json.load(sys.stdin).get("ready") is True else 1)' \
        <<<"${body}"; then
        return 0
      fi
    fi
    sleep 0.1
  done
  echo "stats endpoint did not become ready: ${url}" >&2
  return 1
}

wait_for_idle() {
  local url="$1"
  local deadline=$((SECONDS + TIMEOUT_SECONDS))
  while ((SECONDS < deadline)); do
    local body phase
    body="$(curl --silent --show-error --fail "${url}/bench/stats")"
    phase="$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("phase", ""))' <<<"${body}")"
    case "${phase}" in
      idle) return 0 ;;
      failed) echo "source phase failed: ${body}" >&2; return 1 ;;
    esac
    sleep 0.1
  done
  echo "source phase did not complete: ${url}" >&2
  return 1
}

trigger_phase() {
  local url="$1" run_id="$2" cell_id="$3" pattern="$4" payload="$5" phase="$6"
  curl --silent --show-error --fail \
    -H 'content-type: application/json' \
    -d "{\"runId\":\"${run_id}\",\"cellId\":\"${cell_id}\",\"pattern\":\"${pattern}\",\"payloadBytes\":${payload},\"phase\":\"${phase}\",\"durationMs\":$((DURATION * 1000)),\"requestWindow\":${WINDOW},\"sendConcurrency\":${SEND_CONCURRENCY}}" \
    "${url}/bench/start"
  echo
}

settle_and_capture() {
  local source_url="$1" target_url="$2" target_file="$3"
  local started_ms deadline previous stable stable_needed
  started_ms="$(date +%s%3N)"
  deadline=$((SECONDS + DRAIN_BOUND_MS / 1000))
  previous=""
  stable=0
  stable_needed=$(((COMMAND_SETTLE_MS + 99) / 100))
  while ((SECONDS <= deadline)); do
    local source_body target_body counts
    source_body="$(curl --silent --show-error --fail "${source_url}/bench/stats")"
    target_body="$(curl --silent --show-error --fail "${target_url}/bench/stats")"
    counts="$(python3 -c \
      'import json,sys; a=json.loads(sys.argv[1]); b=json.loads(sys.argv[2]); print(a.get("completed",0),a.get("currentInFlight",0),b.get("received",0),b.get("errors",0))' \
      "${source_body}" "${target_body}")"
    if [[ "${counts}" == "${previous}" ]]; then
      stable=$((stable + 1))
      if ((stable >= stable_needed)); then
        printf '%s\n' "${target_body}" >"${target_file}"
        SETTLE_MS=$(($(date +%s%3N) - started_ms))
        SETTLE_BOUND_HIT=false
        return 0
      fi
    else
      previous="${counts}"
      stable=0
    fi
    sleep 0.1
  done
  curl --silent --show-error --fail "${target_url}/bench/stats" >"${target_file}"
  SETTLE_MS=$(($(date +%s%3N) - started_ms))
  SETTLE_BOUND_HIT=true
  return 1
}

merge_target_stats() {
  local result_file="$1" target_file="$2" drain_ms="$3" bound_hit="$4"
  python3 - "${result_file}" "${target_file}" "${drain_ms}" "${bound_hit}" <<'PY'
import json
import os
import sys

result_path, target_path, drain_ms, bound_hit = sys.argv[1:]
with open(result_path, encoding="utf-8") as handle:
    result = json.load(handle)
with open(target_path, encoding="utf-8") as handle:
    target = json.load(handle)
cell = result["cells"][0]
cell["target_stats"] = {
    "received": target["received"],
    "errors": target["errors"],
    "drainMs": float(drain_ms),
}
cell["drain_ms"] = float(drain_ms)
cell["drain_bound_hit"] = bound_hit == "true"
cell["server_received_at_close"] = target["received"]
temporary = result_path + ".merge"
with open(temporary, "w", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2)
    handle.write("\n")
os.replace(temporary, result_path)
PY
}

verify_request_counts() {
  local result_file="$1"
  python3 - "${result_file}" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as handle:
    value = json.load(handle)
cell = value["cells"][0]
completed = int(cell["completed"])
errors = int(cell.get("errors", 0))
abandoned = int(cell.get("abandoned", 0))
received = int(cell["target_stats"]["received"])
# B may receive requests that A never completed: abandoned after the active window
# (spec 5.2) or failed at A after reaching B. Bounds: completed <= received <=
# completed + errors + abandoned; equality when A saw no error and abandoned nothing.
if received > completed + errors + abandoned or received < completed:
    raise SystemExit(
        f"request count mismatch: source completed={completed} errors={errors} "
        f"abandoned={abandoned}, target received={received}")
if errors == 0 and abandoned == 0 and completed != received:
    raise SystemExit(f"request count mismatch: source completed={completed}, target received={received}")
PY
}

record_unsupported() {
  local run_id="$1" cell_id="$2" pattern="$3" payload="$4"
  python3 - "${OUTROOT}/unsupported.json" "${run_id}" "${cell_id}" "${pattern}" \
    "${payload}" "${UNSUPPORTED_REASON}" <<'PY'
import json
import os
import sys

path, run_id, cell_id, pattern, payload, reason = sys.argv[1:]
if os.path.exists(path):
    with open(path, encoding="utf-8") as handle:
        document = json.load(handle)
else:
    document = {"schema": "with-grpc-unsupported-v1", "cells": []}
document["cells"].append({
    "implementation": "zlink-framework-node",
    "pattern": pattern,
    "payload_size": int(payload),
    "runId": run_id,
    "cellId": cell_id,
    "status": "unsupported",
    "reason": reason,
})
temporary = path + ".write"
with open(temporary, "w", encoding="utf-8") as handle:
    json.dump(document, handle, indent=2)
    handle.write("\n")
os.replace(temporary, path)
PY
  echo "UNSUPPORTED,zlink-framework-node-${pattern},${payload},${UNSUPPORTED_REASON}" | tee -a "${overall_report}"
}

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  load_average="$(cut -d' ' -f1 /proc/loadavg)"
  awk -v load_avg="${load_average}" 'BEGIN { exit !(load_avg < 10.0) }' || {
    echo "load average must be below 10 before build (current ${load_average})" >&2
    exit 1
  }
  npm ci
  npm run build
fi

check_ports_free
mkdir -p "${OUTROOT}"
overall_report="${OUTROOT}/with_grpc_node_${STAMP}.txt"
: >"${overall_report}"
b_pid=""
a_pid=""

cleanup_cell() {
  local status=$?
  set +e
  for pid in "${a_pid}" "${b_pid}"; do
    [[ -n "${pid}" ]] || continue
    kill -TERM -- "-${pid}" >/dev/null 2>&1 || kill -TERM "${pid}" >/dev/null 2>&1 || true
  done
  local deadline=$((SECONDS + 3))
  while ((SECONDS < deadline)); do
    local running=0
    for pid in "${a_pid}" "${b_pid}"; do
      [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1 && running=1
    done
    ((running == 0)) && break
    sleep 0.1
  done
  for pid in "${a_pid}" "${b_pid}"; do
    [[ -n "${pid}" ]] || continue
    kill -KILL -- "-${pid}" >/dev/null 2>&1 || kill -KILL "${pid}" >/dev/null 2>&1 || true
    wait "${pid}" >/dev/null 2>&1 || true
  done
  a_pid=""
  b_pid=""
  set -e
  return "${status}"
}
trap cleanup_cell EXIT

for run in $(seq 1 "${RUNS}"); do
  run_id="${STAMP}-run${run}"
  for impl in "${implementations[@]}"; do
    case "${impl}" in
      grpc-node)
        trigger_url="http://127.0.0.1:5220"
        source_stats_url="http://127.0.0.1:5221"
        target_endpoint="127.0.0.1:5222"
        target_command_endpoint=""
        target_stats_url="http://127.0.0.1:5223"
        target_command=(node grpc-server/main.js --url "${target_endpoint}" --metrics-url "${target_stats_url}")
        ;;
      zlink-node)
        trigger_url="http://127.0.0.1:5225"
        source_stats_url="http://127.0.0.1:5226"
        target_endpoint="tcp://127.0.0.1:5227"
        target_command_endpoint="tcp://127.0.0.1:5228"
        target_stats_url="http://127.0.0.1:5229"
        target_command=(node zlink-raw-server/main.js --endpoint "${target_endpoint}" \
          --command-endpoint "${target_command_endpoint}" --metrics-url "${target_stats_url}")
        ;;
      zlink-framework-node)
        for pattern in "${patterns[@]}"; do
          for payload in "${payloads[@]}"; do
            cell_id="${impl}-${pattern}-${payload}"
            record_unsupported "${run_id}" "${cell_id}" "${pattern}" "${payload}"
          done
        done
        continue
        ;;
    esac

    for pattern in "${patterns[@]}"; do
      for payload in "${payloads[@]}"; do
        cell_id="${impl}-${pattern}-${payload}"
        cell_dir="${OUTROOT}/${cell_id}-run${run}"
        mkdir -p "${cell_dir}"
        target_log="${cell_dir}/target.log"
        source_log="${cell_dir}/source.log"
        target_stats_file="${cell_dir}/target-stats.json"
        echo "[bench] cell=${cell_id} run=${run} starting target B then source A" >&2

        setsid "${target_command[@]}" >"${target_log}" 2>&1 &
        b_pid=$!
        wait_for_stats "${target_stats_url}" 0

        source_args=(
          --implementation "${impl}"
          --scenario "${pattern}"
          --payload-size "${payload}"
          --request-window "${WINDOW}"
          --send-concurrency "${SEND_CONCURRENCY}"
          --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}"
          --warmup "${WARMUP}"
          --drain-bound-ms "${DRAIN_BOUND_MS}"
          --request-timeout-ms "${REQUEST_TIMEOUT_MS}"
          --route-ready-ms "${ROUTE_READY_MS}"
          --trigger-url "${trigger_url}"
          --stats-url "${source_stats_url}"
          --target-endpoint "${target_endpoint}"
          --target-stats-url "${target_stats_url}"
          --raw-socket router
          --output "${cell_dir}"
          --report-file report.txt
        )
        if [[ -n "${target_command_endpoint}" ]]; then
          source_args+=(--target-command-endpoint "${target_command_endpoint}")
        fi
        setsid node client/main.js "${source_args[@]}" >"${source_log}" 2>&1 &
        a_pid=$!
        wait_for_stats "${source_stats_url}" 1

        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" warmup
        wait_for_idle "${source_stats_url}"
        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" active
        wait_for_idle "${source_stats_url}"

        result_file="${cell_dir}/results.json"
        [[ -s "${result_file}" ]] || { echo "source result is missing: ${result_file}" >&2; exit 1; }
        if ! settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}"; then
          echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2
          exit 1
        fi
        merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
        if [[ "${pattern}" == request-* ]]; then verify_request_counts "${result_file}"; fi

        grep '^RESULT,' "${source_log}" | tee -a "${overall_report}"
        cleanup_cell
        wait_for_ports_free
      done
    done
  done
done

echo "[bench] results=${OUTROOT}" >&2
