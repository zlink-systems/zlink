#!/usr/bin/env bash
# C++ server-driven with-grpc bench runner (README §11).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"
bench_init cpp "grpc-cpp,zlink-cpp,zlink-framework-cpp" \
  "request-serial,request-backpressure,send-saturation" "1024,4096"
cd "${HERE}"

WARMUP_SECONDS=5
WARMUP_SEGMENTS=10
# The gate value is a measurement condition and is not relaxed. A previous cell's load average
# lingers for a minute or two, so the runner waits (bounded) for the gate.
LOAD_GATE=2.0
LOAD_GATE_WAIT_SECONDS=600
BUILD_DIR="${HERE}/build"

log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

check_load() {
  local load deadline=$((SECONDS + LOAD_GATE_WAIT_SECONDS))
  while :; do
    load="$(awk '{print $1}' /proc/loadavg)"
    log "loadavg1=${load} gate=${LOAD_GATE}"
    if awk -v measured_load="${load}" -v gate="${LOAD_GATE}" 'BEGIN { exit !(measured_load < gate) }'; then
      return 0
    fi
    ((SECONDS < deadline)) || { echo "load average ${load} stayed above gate ${LOAD_GATE} for ${LOAD_GATE_WAIT_SECONDS}s" >&2; return 1; }
    sleep 10
  done
}

reset_target() {
  curl --silent --show-error --fail --max-time 5 -X POST "$1/bench/reset" >/dev/null
}

# Differs from the shared settle_and_capture: the warmup settle counts messages of any phase,
# the bound is the drain budget A left over, and the stats file wraps B's JSON as {"snapshot":...}.
cpp_settle_and_capture() {
  local source_url="$1" target_url="$2" target_file="$3" target_counter="${4:-received}"
  local bound_ms="${5:-${DRAIN_BOUND_MS}}"
  local started_ms now_ms previous="" stable=0 stable_needed source_body target_body counts
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
print(a.get("completed",0), in_flight, received, b.get("errors",0), b.get("rejected"))
' "${source_body}" "${target_body}" "${target_counter}")"
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

# Differs from the shared merge_target_stats: B's JSON is wrapped in "snapshot", it carries
# "rejected", and the cell's drain is A's own drain plus the runner settle.
cpp_merge_target_stats() {
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
    "rejected": int(target["rejected"]) if target.get("rejected") is not None else None,
    "drainMs": float(drain_ms),
}
cell["server_rejected_count"] = cell["target_stats"]["rejected"]
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

# Differs from the shared verify_request_counts: it also checks send-saturation
# (completed == received + rejected) and B's error count.
cpp_verify_counts() {
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
rejected = cell.get("server_rejected_count")
if cell["pattern"] == "send-saturation" and rejected is None:
    raise SystemExit("target rejection count unavailable: completed == received + rejected cannot be verified")
if rejected is not None:
    rejected = int(rejected)
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
accounted = received + rejected if cell["pattern"] == "send-saturation" else received
if (rejected is not None and rejected < 0) or accounted > submitted:
    raise SystemExit(f"target count mismatch: submitted={submitted} received={received} rejected={rejected}")
if errors == 0 and abandoned == 0 and completed != accounted:
    raise SystemExit(
        f"{cell['pattern']} count mismatch: completed={completed} received={received} rejected={rejected}")
print(f"counts: completed={completed} received={received} server_rejected_count={rejected} "
      f"difference={completed - received}")
PY
}

if [[ "${SKIP_BUILD}" != 1 ]]; then
  bench_require_low_load
  # The C++ Framework resolves its dependencies through its vcpkg manifest, as the Framework's
  # own configure does (scripts/gate/rebuild-dev.sh). gRPC comes from the same manifest through
  # its "bench" feature, so one process links one protobuf.
  REPO_ROOT="$(cd "${HERE}/../../../.." && pwd)"
  [[ -n "${VCPKG_ROOT:-}" && -f "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" ]] || {
    echo "VCPKG_ROOT must point to a bootstrapped vcpkg tree; setup: scripts/gate/check-env.sh" >&2
    exit 1
  }
  export VCPKG_MAX_CONCURRENCY="${VCPKG_MAX_CONCURRENCY:-4}"
  cmake -S "${HERE}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_MANIFEST_DIR="${REPO_ROOT}/framework/languages/cpp" \
    -DVCPKG_MANIFEST_FEATURES=bench \
    -DVCPKG_OVERLAY_PORTS="${REPO_ROOT}/vcpkg/ports"
  cmake --build "${BUILD_DIR}" --parallel 2
fi
for binary in bench_cpp_client bench_cpp_grpc_server bench_cpp_zlink_server bench_cpp_framework_server; do
  [[ -x "${BUILD_DIR}/${binary}" ]] || { echo "missing ${BUILD_DIR}/${binary}; run without SKIP_BUILD=1" >&2; exit 1; }
done

check_ports_free 5280 5299
check_load
mkdir -p "${OUTPUT_DIR}"
a_pid=""
b_pid=""
trap cleanup_cell EXIT

for run in $(seq 1 "${RUNS}"); do
  run_id="${RUN_STAMP}-run${run}"
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
        cell_dir="$(bench_cell_dir "${run}" "${implementation}" "${pattern}" "${payload}")"
        result_file="${cell_dir}/results.json"
        target_stats_file="${cell_dir}/target-stats.json"
        mkdir -p "${cell_dir}"
        log "cell=${cell_id} run=${run}: start target B then source A"

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

        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" \
          "${payload}" warmup "$((WARMUP_SECONDS * 1000))"
        wait_for_idle "${source_stats_url}"
        if ! cpp_settle_and_capture "${source_stats_url}" "${target_stats_url}" \
          "${cell_dir}/warmup-target-stats.json" any; then
          log "warmup settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id} (recorded; cell continues)"
        fi
        reset_target "${target_stats_url}"
        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" \
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
        cpp_settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}" \
          received "${remaining_drain_ms}" || settle_rc=$?
        cpp_merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
        # A bound hit is recorded in drain_bound_hit and the aggregator excludes the cell; the
        # next cell starts a fresh process pair, so the run continues.
        if ((settle_rc != 0)); then
          log "cell settle hit ${DRAIN_BOUND_MS}ms total bound: ${cell_id} (recorded; run continues)"
        fi
        cpp_verify_counts "${result_file}"

        cleanup_cell
        wait_for_ports_free 5280 5299
      done
    done
  done
done

log "results=${OUTPUT_DIR}"
