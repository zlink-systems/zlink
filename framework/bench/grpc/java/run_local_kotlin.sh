#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"
bench_init kotlin "grpc-kotlin,zlink-framework-kotlin" "request-backpressure" "1024"
cd "${HERE}"

select_java_home
export PATH="${JAVA_HOME}/bin:${PATH}"

WARMUP_SECONDS=20

if [[ "${SKIP_BUILD}" != 1 ]]; then
  bench_require_low_load
  "${HERE}/gradlew" --no-daemon --max-workers=1 assemble installDist
fi

GRPC_BIN="${HERE}/grpc-server/build/install/bench-grpc-server/bin/bench-grpc-server"
FW_BIN="${HERE}/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server"
SOURCE_BIN="${HERE}/kotlin-client/build/install/bench-kotlin-client/bin/bench-kotlin-client"
for binary in "${GRPC_BIN}" "${FW_BIN}" "${SOURCE_BIN}"; do
  [[ -x "${binary}" ]] || { echo "missing ${binary}; run without SKIP_BUILD=1" >&2; exit 1; }
done

# Java B ports are shared by contract, so checking both bands also prevents concurrent
# Java and Kotlin measurements.
check_ports_free 5240 5259
check_ports_free 5260 5279
mkdir -p "${OUTPUT_DIR}"
a_pid=""
b_pid=""
trap cleanup_cell EXIT

for run in $(seq 1 "${RUNS}"); do
  run_id="${RUN_STAMP}-run${run}"
  for impl in "${implementations[@]}"; do
    case "${impl}" in
      grpc-kotlin)
        trigger_url="http://127.0.0.1:5260"
        source_stats_url="http://127.0.0.1:5261"
        target_endpoint="127.0.0.1:5242"
        target_stats_url="http://127.0.0.1:5243"
        target_command=("${GRPC_BIN}" --port 5242 --metrics-url "${target_stats_url}")
        ;;
      zlink-framework-kotlin)
        trigger_url="http://127.0.0.1:5272"
        source_stats_url="http://127.0.0.1:5273"
        target_endpoint="tcp://127.0.0.1:5254"
        target_stats_url="http://127.0.0.1:5255"
        target_command=("${FW_BIN}" --endpoint "${target_endpoint}" --metrics-url "${target_stats_url}")
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
        echo "[bench] cell=${cell_id} run=${run}: start Java B then Kotlin A" >&2

        setsid "${target_command[@]}" >"${target_log}" 2>&1 &
        b_pid=$!
        wait_for_stats "${target_stats_url}" 0
        setsid "${SOURCE_BIN}" \
          --implementation "${impl}" --scenario "${pattern}" --payload-size "${payload}" \
          --request-window "${WINDOW}" --send-concurrency "${SEND_CONCURRENCY}" \
          --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}" \
          --warmup-seconds "${WARMUP_SECONDS}" --drain-bound-ms "${DRAIN_BOUND_MS}" \
          --request-timeout-ms "${REQUEST_TIMEOUT_MS}" --route-ready-ms "${ROUTE_READY_MS}" \
          --trigger-url "${trigger_url}" --stats-url "${source_stats_url}" \
          --target-endpoint "${target_endpoint}" --target-stats-url "${target_stats_url}" \
          --run-id "${run_id}" --cell-id "${cell_id}" --raw-socket router \
          --output "${cell_dir}" --report-file report.txt >"${source_log}" 2>&1 &
        a_pid=$!
        wait_for_stats "${source_stats_url}" 1

        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" \
          "${payload}" warmup "$((WARMUP_SECONDS * 1000))"
        wait_for_idle "${source_stats_url}"
        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" \
          "${payload}" active "$((DURATION_SECONDS * 1000))"
        wait_for_idle "${source_stats_url}"

        result_file="${cell_dir}/results.json"
        [[ -s "${result_file}" ]] || { echo "missing source result: ${result_file}" >&2; exit 1; }
        settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}" || {
          echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2; exit 1;
        }
        merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
        verify_request_counts "${result_file}"

        cleanup_cell
        wait_for_ports_free 5240 5259
        wait_for_ports_free 5260 5279
      done
    done
  done
done

echo "[bench] results=${OUTPUT_DIR}" >&2
