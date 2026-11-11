#!/usr/bin/env bash
# C++ server-driven with-grpc bench runner (README §11).
# Optional controls: TARGET_RATE (messages/second), SOURCE_CPUS and TARGET_CPUS (taskset CPU lists).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"
bench_init cpp "grpc-cpp,zlink-cpp,zlink-framework-cpp" \
  "request-serial,request-backpressure,send-saturation" "1024,4096"
cd "${HERE}"

WARMUP_SEGMENTS=10
BUILD_DIR="${HERE}/build"

log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

if [[ "${SKIP_BUILD}" != 1 ]]; then
  "${HERE}/build.sh"
fi
for binary in bench_cpp_client bench_cpp_grpc_server bench_cpp_zlink_server bench_cpp_framework_server; do
  [[ -x "${BUILD_DIR}/${binary}" ]] || { echo "missing ${BUILD_DIR}/${binary}; run without SKIP_BUILD=1" >&2; exit 1; }
done

check_ports_free 5280 5299
mkdir -p "${OUTPUT_DIR}"
a_pid=""
b_pid=""
trap cleanup_cell EXIT

# The paced rate and CPU lists are measurement conditions; keep them with each result.
record_cell_controls() {
  python3 - "$1" "$2" "${TARGET_RATE:-0}" "${SOURCE_CPUS:-}" "${TARGET_CPUS:-}" <<'PY'
import json, os, sys, urllib.request
path, source_url, rate, source_cpus, target_cpus = sys.argv[1:]
with open(path, encoding="utf-8") as handle:
    result = json.load(handle)
with urllib.request.urlopen(source_url + "/bench/stats", timeout=5) as response:
    source = json.load(response)
metadata = result.setdefault("metadata", {})
metadata["submitConcurrency"] = source["submitConcurrency"]
cell = result["cells"][0]
cell.setdefault("streams", source["streams"])
if cell.get("contaminated"):
    cell["submitted"] = source["submitted"]
    cell["completed"] = source["completed"]
    cell["abandoned"] = source["currentInFlight"]
    cell["errors"] = source["errors"]
    cell["client_error_summary"] = source["client_error_summary"]
    cell["client_error_other_count"] = source["client_error_other_count"]
value = float(rate)
metadata["targetRate"] = int(value) if value.is_integer() else value
metadata["sourceCpus"] = source_cpus or "unpinned"
metadata["targetCpus"] = target_cpus or "unpinned"
temporary = path + ".controls"
with open(temporary, "w", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2)
    handle.write("\n")
os.replace(temporary, path)
PY
}

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
      bench_skip_cell "${implementation}" "${pattern}" && continue
      for payload in "${payloads[@]}"; do
        cell_id="${implementation}-${pattern}-${payload}"
        cell_dir="$(bench_cell_dir "${run}" "${implementation}" "${pattern}" "${payload}")"
        result_file="${cell_dir}/results.json"
        target_stats_file="${cell_dir}/target-stats.json"
        mkdir -p "${cell_dir}"
        log "cell=${cell_id} run=${run}: start target B then source A"

        target_process=("${target_command[@]}")
        if [[ -n "${TARGET_CPUS:-}" ]]; then
          target_process=(taskset -c "${TARGET_CPUS}" "${target_process[@]}")
        fi
        setsid "${target_process[@]}" >"${cell_dir}/target.log" 2>&1 &
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
        if [[ -n "${TARGET_RATE:-}" ]]; then
          source_args+=(--target-rate "${TARGET_RATE}")
        fi
        source_process=("${BUILD_DIR}/bench_cpp_client" "${source_args[@]}")
        if [[ -n "${SOURCE_CPUS:-}" ]]; then
          source_process=(taskset -c "${SOURCE_CPUS}" "${source_process[@]}")
        fi
        setsid "${source_process[@]}" >"${cell_dir}/source.log" 2>&1 &
        a_pid=$!
        wait_for_stats "${source_stats_url}" 1

        bench_run_cell "${trigger_url}" "${source_stats_url}" "${target_stats_url}" \
          "${run_id}" "${cell_id}" "${implementation}" "${pattern}" "${payload}" \
          "$((WARMUP_SECONDS * 1000))" "${cell_dir}/results.json" "${target_stats_file}" \
          "${target_stats_url}"
        record_cell_controls "${cell_dir}/results.json" "${source_stats_url}"

        cleanup_cell
        wait_for_ports_free 5280 5299
      done
    done
  done
done

log "results=${OUTPUT_DIR}"
