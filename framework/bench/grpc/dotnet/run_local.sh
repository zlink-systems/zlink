#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIGURATION="${CONFIGURATION:-Release}"
RUN_STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUTPUT="${OUTPUT:-${ROOT_DIR}/../log/dotnet/with_grpc_dotnet_${RUN_STAMP}}"
REPORT_FILE="${REPORT_FILE:-with_grpc_dotnet_${RUN_STAMP}.txt}"
PAYLOAD_SIZES="${PAYLOAD_SIZES:-1024,4096}"
DURATION_SECONDS="${DURATION_SECONDS:-5}"
WARMUP="${WARMUP:-1000}"
REQUEST_WINDOW="${REQUEST_WINDOW:-100}"
SEND_CONCURRENCY="${SEND_CONCURRENCY:-8}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-300}"
COMMAND_SETTLE_MS="${COMMAND_SETTLE_MS:-200}"
DRAIN_BOUND_MS="${DRAIN_BOUND_MS:-30000}"
LATENCY_SAMPLE_LIMIT="${LATENCY_SAMPLE_LIMIT:-200000}"
RAW_SOCKET="${RAW_SOCKET:-router}"

scenario="all"
implementation="all"
while (($# > 0)); do
  case "$1" in
    --scenario)
      scenario="${2:?--scenario requires a value}"
      shift 2
      ;;
    --implementation)
      implementation="${2:?--implementation requires a value}"
      shift 2
      ;;
    *)
      echo "unsupported runner argument: $1" >&2
      exit 2
      ;;
  esac
done

[[ "${REQUEST_WINDOW}" == "100" ]] || { echo "REQUEST_WINDOW must remain 100" >&2; exit 2; }
[[ "${SEND_CONCURRENCY}" == "8" ]] || { echo "SEND_CONCURRENCY must remain 8" >&2; exit 2; }
[[ "${TIMEOUT_SECONDS}" == "300" ]] || { echo "TIMEOUT_SECONDS must remain 300" >&2; exit 2; }
[[ "${RAW_SOCKET}" == "router" ]] || { echo "RAW_SOCKET must remain router" >&2; exit 2; }
[[ "${DURATION_SECONDS}" =~ ^[1-9][0-9]*$ ]] || { echo "DURATION_SECONDS must be a positive integer" >&2; exit 2; }
[[ "${WARMUP}" =~ ^[0-9]+$ ]] || { echo "WARMUP must be a non-negative integer" >&2; exit 2; }
[[ "${COMMAND_SETTLE_MS}" =~ ^[1-9][0-9]*$ ]] || { echo "COMMAND_SETTLE_MS must be a positive integer" >&2; exit 2; }

IFS=',' read -r -a payloads <<<"${PAYLOAD_SIZES}"
for payload in "${payloads[@]}"; do
  [[ "${payload}" == "1024" || "${payload}" == "4096" ]] || {
    echo "PAYLOAD_SIZES entries must be 1024 or 4096" >&2
    exit 2
  }
done

case "${scenario}" in
  all) patterns=(request-serial request-window request-backpressure send-saturation) ;;
  request) patterns=(request-serial request-window request-backpressure) ;;
  request-serial|request-window|request-backpressure|send-saturation) patterns=("${scenario}") ;;
  send|command) patterns=(send-saturation) ;;
  *) echo "unknown scenario: ${scenario}" >&2; exit 2 ;;
esac

case "${implementation}" in
  all) implementations=(grpc-dotnet zlink-dotnet zlink-framework-dotnet) ;;
  grpc-dotnet|zlink-dotnet|zlink-framework-dotnet) implementations=("${implementation}") ;;
  *) echo "unknown implementation: ${implementation}" >&2; exit 2 ;;
esac

check_ports_free() {
  local used
  used="$(ss -H -ltn | awk '
    {
      endpoint=$4
      sub(/^.*:/, "", endpoint)
      if (endpoint ~ /^[0-9]+$/ && endpoint >= 5200 && endpoint <= 5219) print $4
    }')"
  if [[ -n "${used}" ]]; then
    echo "dotnet bench port range 5200-5219 has active listeners: ${used}" >&2
    return 1
  fi
}

wait_for_ports_free() {
  local deadline=$((SECONDS + 30))
  while ((SECONDS < deadline)); do
    if check_ports_free 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  check_ports_free
}

wait_for_stats() {
  local url="$1"
  local require_ready="$2"
  local deadline=$((SECONDS + 30))
  while ((SECONDS < deadline)); do
    local body
    if body="$(curl --silent --show-error --fail "${url}/bench/stats" 2>/dev/null)"; then
      if [[ "${require_ready}" != "1" ]] || python3 -c 'import json,sys; raise SystemExit(0 if json.load(sys.stdin).get("ready") is True else 1)' <<<"${body}"; then
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
      failed)
        echo "source phase failed: ${body}" >&2
        return 1
        ;;
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
    -d "{\"runId\":\"${run_id}\",\"cellId\":\"${cell_id}\",\"pattern\":\"${pattern}\",\"payloadBytes\":${payload},\"phase\":\"${phase}\",\"durationMs\":$((DURATION_SECONDS * 1000)),\"requestWindow\":${REQUEST_WINDOW},\"sendConcurrency\":${SEND_CONCURRENCY}}" \
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
    counts="$(python3 -c 'import json,sys; a=json.loads(sys.argv[1]); b=json.loads(sys.argv[2]); print(a.get("completed",0),a.get("currentInFlight",0),b.get("received",0),b.get("errors",0))' "${source_body}" "${target_body}")"
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
target["drainMs"] = float(drain_ms)
target["drainBoundHit"] = bound_hit == "true"
cell = result["cells"][0]
cell["target_stats"] = target
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
completed = value["cells"][0]["completed"]
received = value["cells"][0]["target_stats"]["received"]
if completed != received:
    raise SystemExit(f"request count mismatch: source completed={completed}, target received={received}")
PY
}

if [[ "${SKIP_BUILD:-0}" != "1" ]]; then
  load_average="$(cut -d' ' -f1 /proc/loadavg)"
  awk -v load="${load_average}" 'BEGIN { exit !(load < 10.0) }' || {
    echo "load average must be below 10 before build (current ${load_average})" >&2
    exit 1
  }
  dotnet build "${ROOT_DIR}/WithGrpcBench.sln" -c "${CONFIGURATION}"
fi

check_ports_free
mkdir -p "${OUTPUT}"
overall_report="${OUTPUT}/${REPORT_FILE}"
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
  done
  for pid in "${a_pid}" "${b_pid}"; do
    [[ -n "${pid}" ]] || continue
    wait "${pid}" >/dev/null 2>&1 || true
  done
  a_pid=""
  b_pid=""
  set -e
  return "${status}"
}
trap cleanup_cell EXIT

for impl in "${implementations[@]}"; do
  case "${impl}" in
    grpc-dotnet)
      trigger_url="http://127.0.0.1:5200"
      source_stats_url="http://127.0.0.1:5201"
      target_endpoint="http://127.0.0.1:5202"
      target_command_endpoint=""
      target_stats_url="http://127.0.0.1:5203"
      target_project="${ROOT_DIR}/GrpcServer/WithGrpcBench.GrpcServer.csproj"
      target_args=(--url "${target_endpoint}" --metrics-url "${target_stats_url}")
      ;;
    zlink-dotnet)
      trigger_url="http://127.0.0.1:5205"
      source_stats_url="http://127.0.0.1:5206"
      target_endpoint="tcp://127.0.0.1:5207"
      target_command_endpoint="tcp://127.0.0.1:5208"
      target_stats_url="http://127.0.0.1:5209"
      target_project="${ROOT_DIR}/ZLinkRawServer/WithGrpcBench.ZLinkRawServer.csproj"
      target_args=(--endpoint "${target_endpoint}" --command-endpoint "${target_command_endpoint}" --metrics-url "${target_stats_url}")
      ;;
    zlink-framework-dotnet)
      trigger_url="http://127.0.0.1:5212"
      source_stats_url="http://127.0.0.1:5213"
      target_endpoint="tcp://127.0.0.1:5214"
      target_command_endpoint=""
      target_stats_url="http://127.0.0.1:5215"
      target_project="${ROOT_DIR}/ZLinkServer/WithGrpcBench.ZLinkServer.csproj"
      target_args=(--endpoint "${target_endpoint}" --metrics-url "${target_stats_url}")
      ;;
  esac

  for pattern in "${patterns[@]}"; do
    for payload in "${payloads[@]}"; do
      cell_id="${impl}-${pattern}-${payload}"
      cell_dir="${OUTPUT}/${cell_id}"
      mkdir -p "${cell_dir}"
      target_log="${cell_dir}/target.log"
      source_log="${cell_dir}/source.log"
      target_stats_file="${cell_dir}/target-stats.json"
      echo "[bench] cell=${cell_id} starting target B then source A" >&2

      setsid dotnet run --no-build -c "${CONFIGURATION}" --project "${target_project}" -- "${target_args[@]}" >"${target_log}" 2>&1 &
      b_pid=$!
      wait_for_stats "${target_stats_url}" 0

      source_args=(
        --implementation "${impl}"
        --scenario "${pattern}"
        --payload-size "${payload}"
        --request-window "${REQUEST_WINDOW}"
        --send-concurrency "${SEND_CONCURRENCY}"
        --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}"
        --warmup "${WARMUP}"
        --drain-bound-ms "${DRAIN_BOUND_MS}"
        --trigger-url "${trigger_url}"
        --stats-url "${source_stats_url}"
        --target-endpoint "${target_endpoint}"
        --target-stats-url "${target_stats_url}"
        --raw-socket "${RAW_SOCKET}"
        --output "${cell_dir}"
        --report-file "report.txt"
        --configuration "${CONFIGURATION}"
        --timeout-seconds "${TIMEOUT_SECONDS}"
      )
      if [[ -n "${target_command_endpoint}" ]]; then
        source_args+=(--target-command-endpoint "${target_command_endpoint}")
      fi
      setsid dotnet run --no-build -c "${CONFIGURATION}" --project "${ROOT_DIR}/Client/WithGrpcBench.Client.csproj" -- "${source_args[@]}" >"${source_log}" 2>&1 &
      a_pid=$!
      wait_for_stats "${source_stats_url}" 1

      trigger_phase "${trigger_url}" "${RUN_STAMP}" "${cell_id}" "${pattern}" "${payload}" warmup
      wait_for_idle "${source_stats_url}"
      trigger_phase "${trigger_url}" "${RUN_STAMP}" "${cell_id}" "${pattern}" "${payload}" active
      wait_for_idle "${source_stats_url}"

      result_file="${cell_dir}/results.json"
      [[ -s "${result_file}" ]] || { echo "source result is missing: ${result_file}" >&2; exit 1; }
      if ! settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}"; then
        echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2
        exit 1
      fi
      merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
      if [[ "${pattern}" == request-* ]]; then
        verify_request_counts "${result_file}"
      fi

      grep '^RESULT,' "${source_log}" | tee -a "${overall_report}"
      cleanup_cell
      wait_for_ports_free
    done
  done
done

echo "[bench] results=${OUTPUT}" >&2
