#!/usr/bin/env bash
# Test-only staged JVM runner.  It intentionally builds a separately compiled
# executable per stage; BENCH_STAGE or another runtime environment value is not
# a stage selector.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stage="${1:?usage: run_staged_jvm.sh core|codec <java|kotlin> <output>}"
language="${2:?usage: run_staged_jvm.sh core|codec <java|kotlin> <output>}"
output="${3:?usage: run_staged_jvm.sh core|codec <java|kotlin> <output>}"
[[ "${stage}" == core || "${stage}" == codec ]] || { echo "stage must be core or codec" >&2; exit 2; }
[[ "${language}" == java || "${language}" == kotlin ]] || { echo "language must be java or kotlin" >&2; exit 2; }

# The staged contract is intentionally independent of the historical 3/10 and
# 10-second cells: one run, 2-second warmup and 5-second active phase.
runs=1
warmup_seconds=2
active_seconds=5
request_bytes=64
response_bytes=4096
send_bytes=4096
logical_payload_bytes=4096

# This is an execution permission, never a stage selector.  The selected stage
# is embedded by Gradle in FixedDiagnosticStage before this process is started.
if [[ "${ZLINK_STAGED_JVM_MEASURE:-}" != 1 ]]; then
  echo "staged JVM: stage=${stage} language=${language} runs=${runs}" \
    "warmupSeconds=${warmup_seconds} activeSeconds=${active_seconds}" \
    "logicalPayloadBytes=${logical_payload_bytes} requestBytes=${request_bytes} responseBytes=${response_bytes}" \
    "sendBytes=${send_bytes}; set ZLINK_STAGED_JVM_MEASURE=1 after approval" >&2
  exit 2
fi

# shellcheck source=../runner_common.sh
source "${here}/runner_common.sh"
# Only the shared report writer is used here. This staged runner has a distinct
# request/reply size contract, so it intentionally does not call bench_runner_args.
# shellcheck source=../../runner_args.sh
source "${here}/../runner_args.sh"
select_java_home
export PATH="${JAVA_HOME}/bin:${PATH}"

window=100
send_concurrency=8
TIMEOUT_SECONDS=300
COMMAND_SETTLE_MS=200
DRAIN_BOUND_MS=30000
REQUEST_TIMEOUT_MS=30000
ROUTE_READY_MS=30000
LATENCY_SAMPLE_LIMIT=200000
WINDOW="${window}"
SEND_CONCURRENCY="${send_concurrency}"
RUN_ID="$(basename "${output}")"
build_root="${output}/build-${stage}"
dist="${build_root}/diagnostics/install/bench-zlink-staged-jvm"
classpath="${dist}/lib/*"
shared_build_root="${output}/build-shared"
core_root="${here}/../../../languages/java"

verify_strict_cell() {
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
drain_bound_hit = bool(cell.get("drain_bound_hit", False))
if (errors or target_errors or abandoned or drain_bound_hit
        or completed != submitted or received != submitted):
    raise SystemExit(
        "staged drain/count mismatch: "
        f"submitted={submitted} completed={completed} received={received} "
        f"errors={errors} targetErrors={target_errors} abandoned={abandoned} "
        f"drainBoundHit={drain_bound_hit}")
PY
}

check_ports_free 5340 5349
mkdir -p "${output}"
runner_log="${output}/runner.log"
: >"${runner_log}"
a_pid=""
b_pid=""
trap cleanup_cell EXIT

"${here}/gradlew" --no-daemon --max-workers=1 :shared:jar \
  -PzlinkBenchBuildDir="${shared_build_root}"
shared_jar="$(find "${shared_build_root}/shared/libs" -maxdepth 1 -name '*.jar' -print -quit)"
[[ -n "${shared_jar}" && -f "${shared_jar}" ]] || { echo "missing bench Shared jar" >&2; exit 1; }
"${core_root}/gradlew" -p "${core_root}" --no-daemon --max-workers=1 :zlink-framework-core:diagnosticJar \
  -PzlinkBenchSharedJar="${shared_jar}"
owner_jar="$(find "${core_root}/zlink-framework-core/build/libs" -maxdepth 1 \
  -name '*-diagnostic.jar' -print -quit)"
[[ -n "${owner_jar}" && -f "${owner_jar}" ]] || { echo "missing owner diagnostic jar" >&2; exit 1; }
"${here}/gradlew" --no-daemon --max-workers=1 :diagnostics:installDist \
  -PbenchDiagnosticStage="${stage}" -PzlinkBenchDiagnosticOwnerJar="${owner_jar}" \
  -PzlinkBenchBuildDir="${build_root}"

target_class="systems.zlink.bench.withgrpc.diagnostics.StagedJavaBench"
if [[ "${language}" == java ]]; then
  source_class="systems.zlink.bench.withgrpc.diagnostics.StagedJavaBench"
else
  source_class="systems.zlink.bench.withgrpc.diagnostics.StagedKotlinBenchKt"
fi

for pattern in request-serial request-backpressure send-saturation; do
  for run in $(seq 1 "${runs}"); do
    cell_id="zlink-staged-${stage}-${language}-${pattern}-r${run}"
    cell_dir="${output}/${cell_id}"
    target_log="${cell_dir}/target.log"
    source_log="${cell_dir}/source.log"
    target_stats_file="${cell_dir}/target-stats.json"
    mkdir -p "${cell_dir}"

    target_endpoint="tcp://127.0.0.1:5340"
    target_command_endpoint="tcp://127.0.0.1:5341"
    target_stats_url="http://127.0.0.1:5342"
    trigger_url="http://127.0.0.1:5343"
    source_stats_url="http://127.0.0.1:5344"
    setsid "${JAVA_HOME}/bin/java" --enable-native-access=ALL-UNNAMED -cp "${classpath}" \
      "${target_class}" --role target --endpoint "${target_endpoint}" \
      --send-endpoint "${target_command_endpoint}" --metrics-url "${target_stats_url}" \
      >"${target_log}" 2>&1 &
    b_pid=$!
    wait_for_stats "${target_stats_url}" 0

    setsid "${JAVA_HOME}/bin/java" --enable-native-access=ALL-UNNAMED -cp "${classpath}" \
      "${source_class}" --role source --implementation "zlink-staged-${language}" \
      --scenario "${pattern}" --payload-size "${logical_payload_bytes}" \
      --request-window "${window}" --send-concurrency "${send_concurrency}" \
      --latency-sample-limit "${LATENCY_SAMPLE_LIMIT}" \
      --warmup-seconds "${warmup_seconds}" --duration-seconds "${active_seconds}" \
      --drain-bound-ms "${DRAIN_BOUND_MS}" --request-timeout-ms "${REQUEST_TIMEOUT_MS}" \
      --route-ready-ms "${ROUTE_READY_MS}" --trigger-url "${trigger_url}" \
      --stats-url "${source_stats_url}" --target-endpoint "${target_endpoint}" \
      --target-command-endpoint "${target_command_endpoint}" --target-stats-url "${target_stats_url}" \
      --run-id "${RUN_ID}" --cell-id "${cell_id}" --raw-socket router \
      --output "${cell_dir}" --report-file report.txt >"${source_log}" 2>&1 &
    a_pid=$!
    wait_for_stats "${source_stats_url}" 1

    trigger_phase "${trigger_url}" "${RUN_ID}" "${cell_id}" "${pattern}" \
      "${logical_payload_bytes}" warmup "$((warmup_seconds * 1000))"
    wait_for_idle "${source_stats_url}"
    trigger_phase "${trigger_url}" "${RUN_ID}" "${cell_id}" "${pattern}" \
      "${logical_payload_bytes}" active "$((active_seconds * 1000))"
    wait_for_idle "${source_stats_url}"

    result_file="${cell_dir}/results.json"
    [[ -s "${result_file}" ]] || { echo "missing source result: ${cell_id}" >&2; exit 1; }
    settle_and_capture "${source_stats_url}" "${target_stats_url}" "${target_stats_file}" || {
      echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id}" >&2; exit 1;
    }
    merge_target_stats "${result_file}" "${target_stats_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
    verify_strict_cell "${result_file}"
    emit_final_results "${result_file}" | tee -a "${runner_log}"
    cleanup_cell
    wait_for_ports_free 5340 5349
  done
done

bench_write_report "${output}"
echo "[bench] staged JVM results=${output}" >&2
