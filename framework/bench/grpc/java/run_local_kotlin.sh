#!/usr/bin/env bash
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${HERE}"
# shellcheck source=runner_common.sh
source "${HERE}/runner_common.sh"

select_java_home
export PATH="${JAVA_HOME}/bin:${PATH}"

RUNS="${RUNS:-3}"
RUN_DEALER="${RUN_DEALER:-0}"
DURATION="${DURATION:-5}"
WARMUP_SECONDS="${WARMUP_SECONDS:-20}"
PAYLOADS="${PAYLOADS:-1024}"
SCENARIO="${SCENARIO:-all}"
IMPLEMENTATION="${IMPLEMENTATION:-all}"
STAMP="${STAMP:-$(date +%Y%m%d_%H%M%S)}"
OUTROOT="${OUTROOT:-${HERE}/../log/java/with_grpc_kotlin_${STAMP}}"
SKIP_BUILD="${SKIP_BUILD:-0}"
WINDOW=100
SEND_CONCURRENCY=8
TIMEOUT_SECONDS=300
COMMAND_SETTLE_MS=200
DRAIN_BOUND_MS=30000
REQUEST_TIMEOUT_MS=30000
ROUTE_READY_MS=30000
LATENCY_SAMPLE_LIMIT=200000

[[ "${RUNS}" =~ ^[1-9][0-9]*$ ]] || { echo "RUNS must be a positive integer" >&2; exit 2; }
[[ "${RUN_DEALER}" == 0 ]] || { echo "RUN_DEALER is unsupported" >&2; exit 2; }
[[ "${DURATION}" =~ ^[1-9][0-9]*$ ]] || { echo "DURATION must be positive" >&2; exit 2; }
[[ "${WARMUP_SECONDS}" =~ ^[1-9][0-9]*$ ]] || { echo "WARMUP_SECONDS must be positive" >&2; exit 2; }
[[ "${PAYLOADS}" == 1024 ]] || { echo "Kotlin auxiliary payload is fixed to 1024" >&2; exit 2; }
case "${SCENARIO}" in
  all|request|request-window) pattern=request-window ;;
  *) echo "Kotlin auxiliary scenario is fixed to request-window" >&2; exit 2 ;;
esac
case "${IMPLEMENTATION}" in
  all) implementations=(grpc-kotlin zlink-framework-kotlin) ;;
  grpc-kotlin|zlink-framework-kotlin) implementations=("${IMPLEMENTATION}") ;;
  *) echo "unknown Kotlin auxiliary implementation: ${IMPLEMENTATION}" >&2; exit 2 ;;
esac

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
mkdir -p "${OUTROOT}"
overall_report="${OUTROOT}/with_grpc_kotlin_${STAMP}.txt"
: >"${overall_report}"
a_pid=""
b_pid=""
trap cleanup_cell EXIT

for run in $(seq 1 "${RUNS}"); do
  run_id="${STAMP}-run${run}"
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

    cell_id="${impl}-${pattern}-1024"
    cell_dir="${OUTROOT}/${cell_id}-run${run}"
    mkdir -p "${cell_dir}"
    target_log="${cell_dir}/target.log"
    source_log="${cell_dir}/source.log"
    target_stats_file="${cell_dir}/target-stats.json"
    echo "[bench] cell=${cell_id} run=${run}: start Java B then Kotlin A" >&2

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
      --run-id "${run_id}" --cell-id "${cell_id}" --raw-socket router \
      --output "${cell_dir}" --report-file report.txt >"${source_log}" 2>&1 &
    a_pid=$!
    wait_for_stats "${source_stats_url}" 1

    trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" 1024 \
      warmup "$((WARMUP_SECONDS * 1000))"
    wait_for_idle "${source_stats_url}"
    trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" 1024 \
      active "$((DURATION * 1000))"
    wait_for_idle "${source_stats_url}"

    result_file="${cell_dir}/results.json"
    [[ -s "${result_file}" ]] || { echo "missing source result: ${result_file}" >&2; exit 1; }
    settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}" || {
      echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2; exit 1;
    }
    merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
    verify_request_counts "${result_file}"
    emit_final_results "${result_file}" | tee -a "${overall_report}"

    cleanup_cell
    wait_for_ports_free 5240 5259
    wait_for_ports_free 5260 5279
  done
done

echo "[bench] results=${OUTROOT}" >&2
