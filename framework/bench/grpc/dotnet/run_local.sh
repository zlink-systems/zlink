#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../runner_common.sh
source "${ROOT_DIR}/../runner_common.sh"
bench_init dotnet "grpc-dotnet,zlink-dotnet,zlink-framework-dotnet" \
  "request-serial,request-backpressure,send-saturation" "1024,4096"

CONFIGURATION=Release
WARMUP_CALLS=1000

if [[ "${SKIP_BUILD}" != 1 ]]; then
  bench_require_low_load
  dotnet build "${ROOT_DIR}/WithGrpcBench.sln" -c "${CONFIGURATION}"
fi

check_ports_free 5200 5219
mkdir -p "${OUTPUT_DIR}"
b_pid=""
a_pid=""
trap cleanup_cell EXIT

for run in $(seq 1 "${RUNS}"); do
  run_id="${RUN_STAMP}-run${run}"
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
        cell_dir="$(bench_cell_dir "${run}" "${impl}" "${pattern}" "${payload}")"
        mkdir -p "${cell_dir}"
        target_log="${cell_dir}/target.log"
        source_log="${cell_dir}/source.log"
        target_stats_file="${cell_dir}/target-stats.json"
        echo "[bench] cell=${cell_id} run=${run}: start B then A" >&2

        setsid dotnet run --no-build -c "${CONFIGURATION}" --project "${target_project}" -- "${target_args[@]}" >"${target_log}" 2>&1 &
        b_pid=$!
        wait_for_stats "${target_stats_url}" 0

        source_args=(
          --implementation "${impl}" --scenario "${pattern}" --payload-size "${payload}"
          --request-window "${WINDOW}" --send-concurrency "${SEND_CONCURRENCY}"
          --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}"
          --warmup "${WARMUP_CALLS}" --drain-bound-ms "${DRAIN_BOUND_MS}"
          --trigger-url "${trigger_url}" --stats-url "${source_stats_url}"
          --target-endpoint "${target_endpoint}" --target-stats-url "${target_stats_url}"
          --raw-socket router
          --output "${cell_dir}" --report-file report.txt
          --configuration "${CONFIGURATION}" --timeout-seconds "${TIMEOUT_SECONDS}"
        )
        if [[ -n "${target_command_endpoint}" ]]; then
          source_args+=(--target-command-endpoint "${target_command_endpoint}")
        fi
        setsid dotnet run --no-build -c "${CONFIGURATION}" --project "${ROOT_DIR}/Client/WithGrpcBench.Client.csproj" -- "${source_args[@]}" >"${source_log}" 2>&1 &
        a_pid=$!
        wait_for_stats "${source_stats_url}" 1

        # The warmup is a call count, so target may still be receiving warmup requests when
        # A goes idle; settle before active so they do not leak into the active count.
        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" warmup "$((DURATION_SECONDS * 1000))"
        wait_for_idle "${source_stats_url}"
        settle_and_capture "${source_stats_url}" "${target_stats_url}" /dev/null || {
          echo "warmup settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2; exit 1;
        }
        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" active "$((DURATION_SECONDS * 1000))"
        wait_for_idle "${source_stats_url}"

        result_file="${cell_dir}/results.json"
        [[ -s "${result_file}" ]] || { echo "missing source result: ${result_file}" >&2; exit 1; }
        settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}" || {
          echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2; exit 1;
        }
        merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
        if [[ "${pattern}" == request-* ]]; then verify_request_counts "${result_file}"; fi

        cleanup_cell
        wait_for_ports_free 5200 5219
      done
    done
  done
done

echo "[bench] results=${OUTPUT_DIR}" >&2
