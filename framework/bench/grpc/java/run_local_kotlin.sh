#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# 인자는 `cd` 전에 읽는다 (README §3.1).
# shellcheck source=../runner_args.sh
source "${HERE}/../runner_args.sh"
bench_runner_args kotlin "$@"
cd "${HERE}"
# shellcheck source=runner_common.sh
source "${HERE}/runner_common.sh"

select_java_home
export PATH="${JAVA_HOME}/bin:${PATH}"

# §10.5의 보조 셀이라 셋 중 좁힌 격자만 돈다: raw binding 행이 없고, 패턴은
# request-window 하나이며, payload는 1024 하나다. 입력 이름은 §3.1과 같고 좁히기만 한다.
case "${IMPLEMENTATION}" in
  all) bench_implementations=(grpc-kotlin zlink-framework-kotlin) ;;
  zlink-kotlin) echo "Kotlin 보조 셀에는 raw binding 행이 없다" >&2; exit 2 ;;
esac
case "${SCENARIO}" in
  all|request) pattern=request-window ;;
  *) echo "Kotlin 보조 셀의 패턴은 request-window 하나다: ${SCENARIO}" >&2; exit 2 ;;
esac
[[ "${PAYLOAD_SIZES}" == 1024 ]] || {
  echo "Kotlin 보조 셀의 payload는 1024 하나다: ${PAYLOAD_SIZES}" >&2; exit 2; }

RUN_ID="$(basename "${OUTPUT}")"
WARMUP_SECONDS="${WARMUP_SECONDS:-20}"
WINDOW=100
SEND_CONCURRENCY=8
TIMEOUT_SECONDS=300
COMMAND_SETTLE_MS=200
DRAIN_BOUND_MS=30000
REQUEST_TIMEOUT_MS=30000
ROUTE_READY_MS=30000
LATENCY_SAMPLE_LIMIT=200000

[[ "${WARMUP_SECONDS}" =~ ^[1-9][0-9]*$ ]] || { echo "WARMUP_SECONDS must be positive" >&2; exit 2; }

if [[ "${SKIP_BUILD}" != 1 ]]; then
  load_average="$(cut -d' ' -f1 /proc/loadavg)"
  awk -v value="${load_average}" 'BEGIN { exit !(value < 10.0) }' || {
    echo "load average must be below 10 before build (current ${load_average})" >&2; exit 1;
  }
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
mkdir -p "${OUTPUT}"
runner_log="${OUTPUT}/runner.log"
: >"${runner_log}"
a_pid=""
b_pid=""
trap cleanup_cell EXIT

for impl in "${bench_implementations[@]}"; do
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

  cell_id="${impl}-${pattern}-1024"
  cell_dir="${OUTPUT}/${cell_id}"
  mkdir -p "${cell_dir}"
  target_log="${cell_dir}/target.log"
  source_log="${cell_dir}/source.log"
  target_stats_file="${cell_dir}/target-stats.json"
  echo "[bench] cell=${cell_id}: start Java B then Kotlin A" >&2

  setsid "${target_command[@]}" >"${target_log}" 2>&1 &
  b_pid=$!
  wait_for_stats "${target_stats_url}" 0
  setsid "${SOURCE_BIN}" \
    --implementation "${impl}" --scenario "${pattern}" --payload-size 1024 \
    --request-window "${WINDOW}" --send-concurrency "${SEND_CONCURRENCY}" \
    --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}" \
    --warmup-seconds "${WARMUP_SECONDS}" --drain-bound-ms "${DRAIN_BOUND_MS}" \
    --request-timeout-ms "${REQUEST_TIMEOUT_MS}" --route-ready-ms "${ROUTE_READY_MS}" \
    --trigger-url "${trigger_url}" --stats-url "${source_stats_url}" \
    --target-endpoint "${target_endpoint}" --target-stats-url "${target_stats_url}" \
    --run-id "${RUN_ID}" --cell-id "${cell_id}" --raw-socket router \
    --output "${cell_dir}" --report-file report.txt >"${source_log}" 2>&1 &
  a_pid=$!
  wait_for_stats "${source_stats_url}" 1

  trigger_phase "${trigger_url}" "${RUN_ID}" "${cell_id}" "${pattern}" 1024 \
    warmup "$((WARMUP_SECONDS * 1000))"
  wait_for_idle "${source_stats_url}"
  trigger_phase "${trigger_url}" "${RUN_ID}" "${cell_id}" "${pattern}" 1024 \
    active "$((DURATION_SECONDS * 1000))"
  wait_for_idle "${source_stats_url}"

  result_file="${cell_dir}/results.json"
  [[ -s "${result_file}" ]] || { echo "missing source result: ${result_file}" >&2; exit 1; }
  settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}" || {
    echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2; exit 1;
  }
  merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
  verify_request_counts "${result_file}"
  emit_final_results "${result_file}" | tee -a "${runner_log}"

  cleanup_cell
  wait_for_ports_free 5240 5259
  wait_for_ports_free 5260 5279
done

bench_write_report "${OUTPUT}"
echo "[bench] results=${OUTPUT}" >&2
