#!/usr/bin/env bash
# C++ server-driven with-grpc bench runner. This script builds nothing.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${SCRIPT_DIR}/build}"
RUN_LABEL="cpp-router-1"
if (($# > 0)) && [[ "$1" != --* ]]; then
  RUN_LABEL="$1"
  shift
fi

STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
LOG_ROOT="${SCRIPT_DIR}/../log/cpp"
LOG_DIR="${OUTPUT_DIR:-${LOG_ROOT}/${STAMP}/${RUN_LABEL}}"
IMPLEMENTATIONS="${IMPLEMENTATIONS:-grpc-cpp,zlink-cpp,zlink-framework-cpp}"
PATTERNS="${PATTERNS:-request-serial,request-backpressure,send-saturation}"
PAYLOAD_SIZES="${PAYLOAD_SIZES:-1024,4096}"
DURATION_SECONDS="${DURATION_SECONDS:-5}"
WARMUP_SECONDS="${WARMUP_SECONDS:-${WARMUP:-5}}"
WARMUP_SEGMENTS="${WARMUP_SEGMENTS:-10}"
REQUEST_WINDOW="${REQUEST_WINDOW:-100}"
SEND_CONCURRENCY="${SEND_CONCURRENCY:-8}"
REQUEST_TIMEOUT_MS="${REQUEST_TIMEOUT_MS:-30000}"
DRAIN_BOUND_MS="${DRAIN_BOUND_MS:-30000}"
COMMAND_SETTLE_MS="${COMMAND_SETTLE_MS:-200}"
TIMEOUT_SECONDS="${TIMEOUT_SECONDS:-300}"
LOAD_GATE="${LOAD_GATE:-2.0}"
LOAD_GATE_WAIT_SECONDS="${LOAD_GATE_WAIT_SECONDS:-600}"
RAW_SOCKET="router"

while (($# > 0)); do
  case "$1" in
    --implementations) IMPLEMENTATIONS="${2:?--implementations requires a value}"; shift 2 ;;
    --patterns) PATTERNS="${2:?--patterns requires a value}"; shift 2 ;;
    --payload-sizes) PAYLOAD_SIZES="${2:?--payload-sizes requires a value}"; shift 2 ;;
    --duration-seconds) DURATION_SECONDS="${2:?--duration-seconds requires a value}"; shift 2 ;;
    --warmup) WARMUP_SECONDS="${2:?--warmup requires a value}"; shift 2 ;;
    --warmup-segments) WARMUP_SEGMENTS="${2:?--warmup-segments requires a value}"; shift 2 ;;
    --request-window) REQUEST_WINDOW="${2:?--request-window requires a value}"; shift 2 ;;
    --send-concurrency) SEND_CONCURRENCY="${2:?--send-concurrency requires a value}"; shift 2 ;;
    --request-timeout-ms) REQUEST_TIMEOUT_MS="${2:?--request-timeout-ms requires a value}"; shift 2 ;;
    --drain-bound-ms) DRAIN_BOUND_MS="${2:?--drain-bound-ms requires a value}"; shift 2 ;;
    --raw-socket) RAW_SOCKET="${2:?--raw-socket requires a value}"; shift 2 ;;
    --output-dir) LOG_DIR="${2:?--output-dir requires a value}"; shift 2 ;;
    *) echo "unsupported runner argument: $1" >&2; exit 2 ;;
  esac
done

[[ "${DURATION_SECONDS}" =~ ^[1-9][0-9]*$ ]] || { echo "--duration-seconds must be a positive integer" >&2; exit 2; }
[[ "${WARMUP_SECONDS}" =~ ^[1-9][0-9]*$ ]] || { echo "--warmup must be a positive integer" >&2; exit 2; }
[[ "${WARMUP_SEGMENTS}" =~ ^[1-9][0-9]*$ ]] || { echo "--warmup-segments must be a positive integer" >&2; exit 2; }
[[ "${REQUEST_WINDOW}" == 100 ]] || { echo "--request-window must remain 100" >&2; exit 2; }
[[ "${SEND_CONCURRENCY}" == 8 ]] || { echo "--send-concurrency must remain 8" >&2; exit 2; }
[[ "${RAW_SOCKET}" == router ]] || { echo "--raw-socket must remain router" >&2; exit 2; }
for value in "${REQUEST_TIMEOUT_MS}" "${DRAIN_BOUND_MS}" "${COMMAND_SETTLE_MS}" "${TIMEOUT_SECONDS}"; do
  [[ "${value}" =~ ^[1-9][0-9]*$ ]] || { echo "timeout and settle values must be positive integers" >&2; exit 2; }
done

IFS=',' read -r -a implementations <<<"${IMPLEMENTATIONS}"
for implementation in "${implementations[@]}"; do
  case "${implementation}" in
    grpc-cpp|zlink-cpp|zlink-framework-cpp) ;;
    *) echo "unknown implementation: ${implementation}" >&2; exit 2 ;;
  esac
done

IFS=',' read -r -a patterns <<<"${PATTERNS}"
for pattern in "${patterns[@]}"; do
  case "${pattern}" in
    request-serial|request-backpressure|send-saturation) ;;
    *) echo "unknown pattern: ${pattern}" >&2; exit 2 ;;
  esac
done

IFS=',' read -r -a payloads <<<"${PAYLOAD_SIZES}"
for payload in "${payloads[@]}"; do
  [[ "${payload}" == 1024 || "${payload}" == 4096 ]] || {
    echo "--payload-sizes entries must be 1024 or 4096" >&2
    exit 2
  }
done

mkdir -p "${LOG_DIR}"
RUNNER_LOG="${LOG_DIR}/runner.log"
OVERALL_REPORT="${LOG_DIR}/report.txt"
: >"${OVERALL_REPORT}"

log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "${RUNNER_LOG}" >&2; }

check_ports_free() {
  local used
  used="$(ss -H -ltn | awk '
    { endpoint=$4; sub(/^.*:/, "", endpoint)
      if (endpoint ~ /^[0-9]+$/ && endpoint >= 5280 && endpoint <= 5299) print $4 }')"
  if [[ -n "${used}" ]]; then
    echo "cpp bench port range 5280-5299 has active listeners: ${used}" >&2
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

# The gate value is a measurement condition and is not relaxed. A previous cell's load average
# lingers for a minute or two, so wait (bounded) for the gate instead of failing the run.
check_load() {
  local load deadline=$((SECONDS + LOAD_GATE_WAIT_SECONDS))
  while :; do
    load="$(awk '{print $1}' /proc/loadavg)"
    log "loadavg1=${load} gate=${LOAD_GATE}"
    printf '%s loadavg1=%s gate=%s at=%s\n' "${RUN_LABEL}" "${load}" "${LOAD_GATE}" "$(date -Is)" \
      >>"${LOG_DIR}/load-gates.txt"
    if awk -v measured_load="${load}" -v gate="${LOAD_GATE}" 'BEGIN { exit !(measured_load < gate) }'; then
      return 0
    fi
    ((SECONDS < deadline)) || { echo "load average ${load} stayed above gate ${LOAD_GATE} for ${LOAD_GATE_WAIT_SECONDS}s" >&2; return 1; }
    sleep 10
  done
}

wait_for_stats() {
  local url="$1" require_ready="$2" deadline=$((SECONDS + 30)) body
  while ((SECONDS < deadline)); do
    if body="$(curl --silent --show-error --fail --max-time 5 "${url}/bench/stats" 2>/dev/null)"; then
      if [[ "${require_ready}" != 1 ]] || python3 -c \
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
  local url="$1" deadline=$((SECONDS + TIMEOUT_SECONDS)) body phase
  while ((SECONDS < deadline)); do
    if ! body="$(curl --silent --show-error --fail --max-time 5 "${url}/bench/stats")"; then
      sleep 0.1
      continue
    fi
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
  local url="$1" run_id="$2" cell_id="$3" pattern="$4" payload="$5" phase="$6" duration_ms="$7"
  python3 - "${run_id}" "${cell_id}" "${pattern}" "${payload}" "${phase}" \
    "${duration_ms}" "${REQUEST_WINDOW}" "${SEND_CONCURRENCY}" <<'PY' |
import json, sys
run_id, cell_id, pattern, payload, phase, duration, window, concurrency = sys.argv[1:]
print(json.dumps({
    "runId": run_id, "cellId": cell_id, "pattern": pattern,
    "payloadBytes": int(payload), "phase": phase, "durationMs": int(duration),
    "requestWindow": int(window), "sendConcurrency": int(concurrency),
}, separators=(",", ":")))
PY
    curl --silent --show-error --fail --max-time 5 -H 'content-type: application/json' \
      --data-binary @- "${url}/bench/start"
  echo
}

reset_target() {
  local url="$1"
  curl --silent --show-error --fail --max-time 5 -X POST "${url}/bench/reset" >/dev/null
}

settle_and_capture() {
  local source_url="$1" target_url="$2" target_file="$3" target_counter="${4:-received}"
  local bound_ms="${5:-${DRAIN_BOUND_MS}}"
  local started_ms now_ms previous="" stable=0 stable_needed source_body target_body counts in_flight
  started_ms="$(date +%s%3N)"
  stable_needed=$(((COMMAND_SETTLE_MS + 99) / 100))
  while :; do
    source_body="$(curl --silent --show-error --fail --max-time 5 "${source_url}/bench/stats")"
    target_body="$(curl --silent --show-error --fail --max-time 5 "${target_url}/bench/stats")"
    counts="$(python3 -c '
import json,sys
a=json.loads(sys.argv[1]); b=json.loads(sys.argv[2])
in_flight=a.get("currentInFlight", a.get("inFlight", 0))
received=(b.get("anyPhaseMessages", 0) if sys.argv[3] == "any"
          else b.get("received", b.get("activeMessages", 0)))
print(a.get("completed",0), in_flight, received, b.get("errors",0))
' "${source_body}" "${target_body}" "${target_counter}")"
    in_flight="$(awk '{print $2}' <<<"${counts}")"
    # Settle = counts unchanged for COMMAND_SETTLE_MS (spec 3). Abandoned operations keep the
    # source in-flight count above zero after the window; they are a recorded result, not a
    # reason to wait for the bound.
    if [[ "${counts}" == "${previous}" ]]; then
      stable=$((stable + 1))
      if ((stable >= stable_needed)); then
        printf '{"snapshot":%s}\n' "${target_body}" >"${target_file}"
        SETTLE_MS=$(($(date +%s%3N) - started_ms))
        SETTLE_BOUND_HIT=false
        return 0
      fi
    else
      previous="${counts}"
      stable=0
    fi
    now_ms="$(date +%s%3N)"
    if ((now_ms - started_ms >= bound_ms)); then break; fi
    sleep 0.1
  done
  target_body="$(curl --silent --show-error --fail --max-time 5 "${target_url}/bench/stats")"
  printf '{"snapshot":%s}\n' "${target_body}" >"${target_file}"
  SETTLE_MS=$(($(date +%s%3N) - started_ms))
  SETTLE_BOUND_HIT=true
  return 1
}

merge_target_stats() {
  local result_file="$1" target_file="$2" drain_ms="$3" bound_hit="$4"
  python3 - "${result_file}" "${target_file}" "${drain_ms}" "${bound_hit}" <<'PY'
import json, os, sys
result_path, target_path, drain_ms, bound_hit = sys.argv[1:]
with open(result_path, encoding="utf-8") as handle:
    result = json.load(handle)
with open(target_path, encoding="utf-8") as handle:
    target = json.load(handle)["snapshot"]
cell = result["cells"][0]
received = target.get("received", target.get("activeMessages"))
if received is None:
    raise SystemExit("target stats missing received")
cell["target_stats"] = {
    "received": int(received),
    "errors": int(target.get("errors", 0)),
    "drainMs": float(drain_ms),
}
source_drain_ms = float(cell.get("source_drain_ms", cell.get("drain_ms", 0.0)))
cell["source_drain_ms"] = source_drain_ms
cell["runner_settle_ms"] = float(drain_ms)
cell["drain_ms"] = source_drain_ms + float(drain_ms)
cell["target_stats"]["drainMs"] = cell["drain_ms"]
cell["drain_bound_hit"] = bool(cell.get("drain_bound_hit", False)) or bound_hit == "true"
temporary = result_path + ".merge"
with open(temporary, "w", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2)
    handle.write("\n")
os.replace(temporary, result_path)
PY
}

verify_counts() {
  python3 - "$1" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    cell = json.load(handle)["cells"][0]
submitted = int(cell["submitted"])
completed = int(cell["completed"])
errors = int(cell.get("errors", 0))
abandoned = int(cell.get("abandoned", 0))
received = int(cell["target_stats"]["received"])
target_errors = int(cell["target_stats"].get("errors", 0))
if submitted != completed + errors + abandoned:
    raise SystemExit(
        f"source count mismatch: submitted={submitted} completed={completed} "
        f"errors={errors} abandoned={abandoned}")
if target_errors != 0:
    raise SystemExit(f"target reported errors={target_errors}")
if cell["pattern"].startswith("request-") and not completed <= received <= completed + errors + abandoned:
    raise SystemExit(
        f"request count mismatch: completed={completed} received={received} "
        f"errors={errors} abandoned={abandoned}")
if errors == 0 and abandoned == 0 and completed != received:
    raise SystemExit(
        f"{cell['pattern']} count mismatch: completed={completed} received={received}")
PY
}

emit_final_results() {
  python3 - "$1" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    cell = json.load(handle)["cells"][0]
fields = {
    "throughput": "throughput_per_second", "bandwidth": "bandwidth_mb_s",
    "latency": "latency_mean_ms", "latency_p95": "latency_p95_ms",
    "latency_p99": "latency_p99_ms", "client_cpu_percent": "client_cpu_percent",
    "client_memory_mb": "client_memory_mb", "server_cpu_percent": "server_cpu_percent",
    "server_memory_mb": "server_memory_mb",
}
scenario = f'{cell["implementation"]}-{cell["pattern"]}'
for metric, field in fields.items():
    print(f'RESULT,current,{scenario},local,{cell["payload_size"]},{metric},{float(cell[field]):.3f}')
PY
}

a_pid=""
b_pid=""
cleanup_cell() {
  local status=$?
  set +e
  for pid in "${a_pid}" "${b_pid}"; do
    [[ -n "${pid}" ]] || continue
    kill -TERM -- "-${pid}" >/dev/null 2>&1 || kill -TERM "${pid}" >/dev/null 2>&1 || true
  done
  local deadline=$((SECONDS + 3)) running pid
  while ((SECONDS < deadline)); do
    running=0
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

check_ports_free
check_load

for implementation in "${implementations[@]}"; do
  case "${implementation}" in
    grpc-cpp)
      trigger_port=5280; source_stats_port=5281
      target_endpoint="127.0.0.1:5282"; target_command_endpoint=""; target_stats_port=5283
      target_command=("${BUILD_DIR}/bench_cpp_grpc_server" --endpoint "${target_endpoint}" --stats-port "${target_stats_port}")
      ;;
    zlink-cpp)
      trigger_port=5285; source_stats_port=5286
      target_endpoint="tcp://127.0.0.1:5287"; target_command_endpoint="tcp://127.0.0.1:5288"; target_stats_port=5289
      target_command=("${BUILD_DIR}/bench_cpp_zlink_server" --endpoint "${target_endpoint}" \
        --command-endpoint "${target_command_endpoint}" --stats-port "${target_stats_port}")
      ;;
    zlink-framework-cpp)
      trigger_port=5292; source_stats_port=5293
      target_endpoint="tcp://127.0.0.1:5294"; target_command_endpoint=""; target_stats_port=5295
      target_command=("${BUILD_DIR}/bench_cpp_framework_server" --endpoint "${target_endpoint}" --stats-port "${target_stats_port}")
      ;;
  esac
  trigger_url="http://127.0.0.1:${trigger_port}"
  source_stats_url="http://127.0.0.1:${source_stats_port}"
  target_stats_url="http://127.0.0.1:${target_stats_port}"

  for pattern in "${patterns[@]}"; do
    for payload in "${payloads[@]}"; do
      cell_id="${implementation}-${pattern}-${payload}"
      cell_dir="${LOG_DIR}/${cell_id}"
      result_file="${cell_dir}/results.json"
      target_stats_file="${cell_dir}/target-stats.json"
      mkdir -p "${cell_dir}"
      log "cell=${cell_id}: start target B then source A"

      setsid "${target_command[@]}" >"${cell_dir}/target.log" 2>&1 &
      b_pid=$!
      wait_for_stats "${target_stats_url}" 0

      source_args=(
        --implementation "${implementation}" --pattern "${pattern}" --payload-size "${payload}"
        --trigger-port "${trigger_port}" --stats-port "${source_stats_port}"
        --target-stats-port "${target_stats_port}" --endpoint "${target_endpoint}"
        --output-file "${result_file}" --warmup-seconds "${WARMUP_SECONDS}"
        --warmup-segments "${WARMUP_SEGMENTS}" --request-timeout-ms "${REQUEST_TIMEOUT_MS}"
        --drain-bound-ms "${DRAIN_BOUND_MS}"
      )
      if [[ -n "${target_command_endpoint}" ]]; then
        source_args+=(--command-endpoint "${target_command_endpoint}")
      fi
      setsid "${BUILD_DIR}/bench_cpp_client" "${source_args[@]}" >"${cell_dir}/source.log" 2>&1 &
      a_pid=$!
      wait_for_stats "${source_stats_url}" 1

      trigger_phase "${trigger_url}" "${STAMP}-${RUN_LABEL}" "${cell_id}" "${pattern}" \
        "${payload}" warmup "$((WARMUP_SECONDS * 1000))"
      wait_for_idle "${source_stats_url}"
      if ! settle_and_capture "${source_stats_url}" "${target_stats_url}" \
        "${cell_dir}/warmup-target-stats.json" any; then
        log "warmup settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id} (recorded; cell continues)"
      fi
      reset_target "${target_stats_url}"
      trigger_phase "${trigger_url}" "${STAMP}-${RUN_LABEL}" "${cell_id}" "${pattern}" \
        "${payload}" active "$((DURATION_SECONDS * 1000))"
      wait_for_idle "${source_stats_url}"

      [[ -s "${result_file}" ]] || { echo "missing source result: ${result_file}" >&2; exit 1; }
      source_drain_ms="$(python3 - "${result_file}" <<'PY'
import json, math, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    cell = json.load(handle)["cells"][0]
print(max(0, math.ceil(float(cell.get("drain_ms", 0)))))
PY
)"
      remaining_drain_ms=$((DRAIN_BOUND_MS - source_drain_ms))
      ((remaining_drain_ms > 0)) || remaining_drain_ms=0
      settle_rc=0
      settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}" \
        received "${remaining_drain_ms}" || settle_rc=$?
      merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
      # A bound hit is recorded in target_stats (drainBoundHit) and the cell is excluded by the
      # aggregator's rules; the next cell starts a fresh process pair, so the run continues.
      if ((settle_rc != 0)); then
        log "cell settle hit ${DRAIN_BOUND_MS}ms total bound: ${cell_id} (recorded; run continues)"
      fi
      verify_counts "${result_file}"
      emit_final_results "${result_file}" | tee -a "${OVERALL_REPORT}"

      cleanup_cell
      wait_for_ports_free
    done
  done
done

log "results=${LOG_DIR}"
