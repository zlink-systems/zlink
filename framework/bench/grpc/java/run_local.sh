#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../../.." && pwd)"
# 인자는 `cd` 전에 읽는다. 상대 경로 OUTPUT을 이 디렉터리 기준으로 풀면 호출자가 지정한
# 곳이 아닌 곳에 결과가 생긴다.
# shellcheck source=../runner_args.sh
source "${HERE}/../runner_args.sh"
bench_runner_args java "$@"
cd "${HERE}"
# shellcheck source=runner_common.sh
source "${HERE}/runner_common.sh"

select_java_home
export PATH="${JAVA_HOME}/bin:${PATH}"

RUN_ID="$(basename "${OUTPUT}")"
WINDOW=100
SEND_CONCURRENCY=8
TIMEOUT_SECONDS=300
COMMAND_SETTLE_MS=200
DRAIN_BOUND_MS=30000
REQUEST_TIMEOUT_MS=30000
ROUTE_READY_MS=30000
LATENCY_SAMPLE_LIMIT=200000


if [[ "${SKIP_BUILD}" != 1 ]]; then
  load_average="$(cut -d' ' -f1 /proc/loadavg)"
  awk -v value="${load_average}" 'BEGIN { exit !(value < 10.0) }' || {
    echo "load average must be below 10 before build (current ${load_average})" >&2; exit 1;
  }
  "${HERE}/gradlew" --no-daemon --max-workers=1 assemble installDist
fi

GRPC_BIN="${HERE}/grpc-server/build/install/bench-grpc-server/bin/bench-grpc-server"
RAW_BIN="${HERE}/zlink-raw-server/build/install/bench-zlink-raw-server/bin/bench-zlink-raw-server"
FW_BIN="${HERE}/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server"
SOURCE_BIN="${HERE}/client/build/install/bench-client/bin/bench-client"
for binary in "${GRPC_BIN}" "${RAW_BIN}" "${FW_BIN}" "${SOURCE_BIN}"; do
  [[ -x "${binary}" ]] || { echo "missing ${binary}; run without SKIP_BUILD=1" >&2; exit 1; }
done

check_ports_free 5240 5259
mkdir -p "${OUTPUT}"
runner_log="${OUTPUT}/runner.log"
: >"${runner_log}"
a_pid=""
b_pid=""
trap cleanup_cell EXIT

for impl in "${bench_implementations[@]}"; do
  case "${impl}" in
    grpc-java)
      trigger_url="http://127.0.0.1:5240"
      source_stats_url="http://127.0.0.1:5241"
      target_endpoint="127.0.0.1:5242"
      target_command_endpoint=""
      target_stats_url="http://127.0.0.1:5243"
      target_command=("${GRPC_BIN}" --port 5242 --metrics-url "${target_stats_url}")
      ;;
    zlink-java)
      trigger_url="http://127.0.0.1:5245"
      source_stats_url="http://127.0.0.1:5246"
      target_endpoint="tcp://127.0.0.1:5247"
      target_command_endpoint="tcp://127.0.0.1:5248"
      target_stats_url="http://127.0.0.1:5249"
      target_command=("${RAW_BIN}" --endpoint "${target_endpoint}" \
        --command-endpoint "${target_command_endpoint}" --metrics-url "${target_stats_url}")
      ;;
    zlink-framework-java)
      trigger_url="http://127.0.0.1:5252"
      source_stats_url="http://127.0.0.1:5253"
      target_endpoint="tcp://127.0.0.1:5254"
      target_command_endpoint=""
      target_stats_url="http://127.0.0.1:5255"
      target_command=("${FW_BIN}" --endpoint "${target_endpoint}" --metrics-url "${target_stats_url}")
      ;;
  esac

for pattern in "${bench_patterns[@]}"; do
  for payload in "${bench_payloads[@]}"; do
      cell_id="${impl}-${pattern}-${payload}"
      cell_dir="${OUTPUT}/${cell_id}"
      mkdir -p "${cell_dir}"
      target_log="${cell_dir}/target.log"
      source_log="${cell_dir}/source.log"
      target_stats_file="${cell_dir}/target-stats.json"
      echo "[bench] cell=${cell_id}: start B then A" >&2

      setsid "${target_command[@]}" >"${target_log}" 2>&1 &
      b_pid=$!
      wait_for_stats "${target_stats_url}" 0

      source_args=(
        --implementation "${impl}" --scenario "${pattern}" --payload-size "${payload}"
        --request-window "${WINDOW}" --send-concurrency "${SEND_CONCURRENCY}"
        --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}"
        --warmup-seconds "${WARMUP_SECONDS}" --drain-bound-ms "${DRAIN_BOUND_MS}"
        --request-timeout-ms "${REQUEST_TIMEOUT_MS}" --route-ready-ms "${ROUTE_READY_MS}"
        --trigger-url "${trigger_url}" --stats-url "${source_stats_url}"
        --target-endpoint "${target_endpoint}" --target-stats-url "${target_stats_url}"
        --run-id "${RUN_ID}" --cell-id "${cell_id}" --raw-socket router
        --output "${cell_dir}" --report-file report.txt
      )
      if [[ -n "${target_command_endpoint}" ]]; then
        source_args+=(--target-command-endpoint "${target_command_endpoint}")
      fi
      setsid "${SOURCE_BIN}" "${source_args[@]}" >"${source_log}" 2>&1 &
      a_pid=$!
      wait_for_stats "${source_stats_url}" 1

      trigger_phase "${trigger_url}" "${RUN_ID}" "${cell_id}" "${pattern}" \
        "${payload}" warmup "$((WARMUP_SECONDS * 1000))"
      wait_for_idle "${source_stats_url}"
      trigger_phase "${trigger_url}" "${RUN_ID}" "${cell_id}" "${pattern}" \
        "${payload}" active "$((DURATION_SECONDS * 1000))"
      wait_for_idle "${source_stats_url}"

      result_file="${cell_dir}/results.json"
      [[ -s "${result_file}" ]] || { echo "missing source result: ${result_file}" >&2; exit 1; }
      settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}" || {
        echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2; exit 1;
      }
      merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
      if [[ "${pattern}" == request-* ]]; then verify_request_counts "${result_file}"; fi
      emit_final_results "${result_file}" | tee -a "${runner_log}"

      cleanup_cell
      wait_for_ports_free 5240 5259
    done
  done
done

bench_write_report "${OUTPUT}"
echo "[bench] results=${OUTPUT}" >&2
