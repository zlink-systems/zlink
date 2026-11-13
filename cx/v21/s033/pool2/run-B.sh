#!/usr/bin/env bash
# C++ server-driven with-grpc bench runner (README §11).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/runner-common.sh"
bench_init cpp "grpc-cpp,zlink-cpp,zlink-framework-cpp" \
  "request-serial,request-backpressure,send-saturation" "1024,4096"
cd "${HERE}"

WARMUP_SEGMENTS=10
BUILD_DIR="${BUILD_DIR:?}"

log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

if [[ "${SKIP_BUILD}" != 1 ]]; then
  "${HERE}/build.sh"
fi
for binary in bench_cpp_client bench_cpp_framework_server bench_cpp_pm_server; do
  [[ -x "${BUILD_DIR}/${binary}" ]] || { echo "missing ${BUILD_DIR}/${binary}; run without SKIP_BUILD=1" >&2; exit 1; }
done

check_ports_free 5280 5299
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
        target_command=("${BUILD_DIR}/${FRAMEWORK_TARGET:-bench_cpp_framework_server}" --endpoint "${target_endpoint}" --stats-port "${target_stats_port}")
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

        if [[ -n "${QUEUE_LOG_DIR:-}" ]]; then
          mkdir -p "${QUEUE_LOG_DIR}"
          target_command=(env "ZLINK_PERF_COMPLETION_QUEUE_LOG=${QUEUE_LOG_DIR}/target-queue.tsv" "${target_command[@]}")
        fi
        b_pid=$(python3 "${HERE}/processes.py" launch "${cell_dir}/target.log" taskset -c 8-15 "${target_command[@]}")
        wait_for_stats "${target_stats_url}" 0
        if [[ ${PM:-0} == 1 ]]; then
          # Fixed-RID MeshNode publication waits the reclaim margin (c5a9d26f41); stats port open is not Framework readiness.
          pm_ready=0
          for _ in $(seq 120); do
            if curl --fail --silent --max-time 10 "${target_stats_url}/pm/request" > "${cell_dir}/spot-setup.json"; then pm_ready=1; break; fi
            sleep 0.5
          done
          ((pm_ready)) || { echo "PM Spot not ready after 60 s" >&2; exit 22; }
        fi

        source_args=(
          --implementation "${implementation}" --pattern "${pattern}" --payload-size "${payload}"
          --trigger-port "${trigger_port}" --stats-port "${source_stats_port}"
          --target-stats-port "${target_stats_port}" --endpoint "${target_endpoint}"
          --output-file "${result_file}" --warmup-seconds "${WARMUP_SECONDS}"
          --warmup-segments "${WARMUP_SEGMENTS}" --request-timeout-ms "${REQUEST_TIMEOUT_MS}"
          --drain-bound-ms "${DRAIN_BOUND_MS}"
        )
        if [[ -n "${TARGET_RATE:-}" ]]; then
          source_args+=(--target-rate "${TARGET_RATE}")
        fi
        if [[ -n "${target_command_endpoint}" ]]; then
          source_args+=(--command-endpoint "${target_command_endpoint}")
        fi
        source_command=("${BUILD_DIR}/bench_cpp_client" "${source_args[@]}")
        if [[ -n "${QUEUE_LOG_DIR:-}" ]]; then
          source_command=(env "ZLINK_PERF_COMPLETION_QUEUE_LOG=${QUEUE_LOG_DIR}/source-queue.tsv" "${source_command[@]}")
        fi
        a_pid=$(python3 "${HERE}/processes.py" launch "${cell_dir}/source.log" taskset -c 0-7 "${source_command[@]}")
        wait_for_stats "${source_stats_url}" 1

        bench_run_cell "${trigger_url}" "${source_stats_url}" "${target_stats_url}" \
          "${run_id}" "${cell_id}" "${implementation}" "${pattern}" "${payload}" \
          "$((WARMUP_SECONDS * 1000))" "${cell_dir}/results.json" "${target_stats_file}" \
          "${target_stats_url}"
        python3 - "${cell_dir}/results.json" "${TARGET_RATE:-0}" <<'PYMETA'
import json,os,sys
p=sys.argv[1]; d=json.loads(open(p).read()); m=d.setdefault("metadata",{})
m.update(targetRate=sys.argv[2],sourceCpus="0-7",targetCpus="8-15")
t=p+".controls"; open(t,"w").write(json.dumps(d,indent=2)+"\n"); os.replace(t,p)
PYMETA

        cleanup_cell
        wait_for_ports_free 5280 5299
      done
    done
  done
done

log "results=${OUTPUT_DIR}"
