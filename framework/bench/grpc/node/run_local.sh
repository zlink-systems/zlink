#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../../.." && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"
bench_init node "grpc-node,zlink-node,zlink-framework-node" \
  "request-serial,request-backpressure,send-saturation" "1024,4096"
cd "${HERE}"

export NODE_PATH="${REPO}/framework/languages/node/node_modules${NODE_PATH:+:${NODE_PATH}}"

# Requests sent before the measured window (README §8.2).
WARMUP=1000
NODE_PORT_LOW=5220
NODE_PORT_HIGH=5239

if [[ "${SKIP_BUILD}" != 1 ]]; then
  "${HERE}/build.sh"
fi

check_ports_free "${NODE_PORT_LOW}" "${NODE_PORT_HIGH}"
mkdir -p "${OUTPUT_DIR}"
b_pid=""
a_pid=""

trap cleanup_cell EXIT

for run in $(seq 1 "${RUNS}"); do
  run_id="${RUN_STAMP}-run${run}"
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
        trigger_url="http://127.0.0.1:5232"
        source_stats_url="http://127.0.0.1:5233"
        target_endpoint="tcp://127.0.0.1:5234"
        target_command_endpoint=""
        target_stats_url="http://127.0.0.1:5235"
        target_command=(node zlink-framework-server/main.js --endpoint "${target_endpoint}" \
          --metrics-url "${target_stats_url}")
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

        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" warmup \
          "$((DURATION_SECONDS * 1000))"
        wait_for_idle "${source_stats_url}"
        trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" active \
          "$((DURATION_SECONDS * 1000))"
        wait_for_idle "${source_stats_url}"

        result_file="${cell_dir}/results.json"
        [[ -s "${result_file}" ]] || { echo "source result is missing: ${result_file}" >&2; exit 1; }
        if ! settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}"; then
          echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2
          exit 1
        fi
        merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
        if [[ "${pattern}" == request-* ]]; then verify_request_counts "${result_file}"; fi

        cleanup_cell
        wait_for_ports_free "${NODE_PORT_LOW}" "${NODE_PORT_HIGH}"
      done
    done
  done
done

echo "[bench] results=${OUTPUT_DIR}" >&2
