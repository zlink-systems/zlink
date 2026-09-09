#!/usr/bin/env bash
# One consolidated verification span for the with-grpc bench: one run per
# language, all five at the same commit, inside a single measured window.
#
# The five language rows were each measured in their own gated span at five
# different commits. Nothing outside doc/ and the bench directories changed
# between them, but that is an argument rather than a measurement, and the
# cross-language conclusions rest on comparing across those spans. This span
# tests whether that comparison holds. It is not here to produce new medians.
#
# plan 3.2 / G7: this script BUILDS NOTHING. Build first, separately, then run.
# It holds /tmp/zlink-perf.lock for the whole span, re-reads the 1 minute load
# average before EVERY run, and records each reading -- "the load was fine" is
# a claim the result has to be able to support.
#
#   DRY_RUN=1 stubs every bench invocation so the orchestration can be
#   rehearsed without measuring anything.
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
export RUN_STAMP="${STAMP}"
SPAN_DIR="${REPO}/framework/bench/tools/log/verify-${STAMP}"
DRY_RUN="${DRY_RUN:-0}"
LOAD_GATE="${LOAD_GATE:-2.0}"

mkdir -p "${SPAN_DIR}"
TIMELINE="${SPAN_DIR}/timeline.txt"
GATES="${SPAN_DIR}/load-gates.txt"

note () { printf '%s %s\n' "$(date -Is)" "$*" | tee -a "${TIMELINE}"; }

DOTNET_DIR="${REPO}/framework/languages/dotnet/bench/with-grpc"
NODE_DIR="${REPO}/framework/languages/node/bench/with-grpc"
JAVA_DIR="${REPO}/framework/languages/java/bench/with-grpc"
CPP_DIR="${REPO}/framework/languages/cpp/bench/with-grpc"
C_DIR="${REPO}/bindings/c/bench/with_grpc"

# ---------------------------------------------------------------- preflight --

# A leftover server from an earlier run would take the port band and make the
# recorded endpoint disagree with the measured one. Every band is checked once,
# before the lock is useful for anything.
preflight_ports () {
  local busy=0 port
  for port in 5071 5072 5073 5074 5075 5076 5077 \
              5081 5082 5083 5084 5085 5086 5087 \
              5091 5092 5093 5094 5095 5096 5097 \
              5101 5102 5103 5104 5105 5106 5107 \
              5111 5112 5113 5114 5115 5116 5117; do
    if ss -ltn "sport = :${port}" 2>/dev/null | grep -q LISTEN; then
      note "PREFLIGHT: port ${port} is in use"
      busy=1
    fi
  done
  return ${busy}
}

# ---------------------------------------------------------------- load gate --

wait_for_load () {
  local label="$1" reading waited=0
  reading="$(awk '{print $1}' /proc/loadavg)"
  note "gate ${label}: loadavg1=${reading} gate=${LOAD_GATE}"
  echo "${label} first=${reading} gate=${LOAD_GATE} at=$(date -Is)" >> "${GATES}"
  while ! awk -v g="${LOAD_GATE}" '{exit !($1 < g)}' /proc/loadavg; do
    if [[ ${waited} -ge 600 ]]; then
      note "gate ${label}: load never fell below ${LOAD_GATE} in 600s"
      echo "${label} FAILED waited=${waited}s" >> "${GATES}"
      return 1
    fi
    sleep 10
    waited=$((waited + 10))
  done
  reading="$(awk '{print $1}' /proc/loadavg)"
  note "gate ${label}: start at loadavg1=${reading} after ${waited}s wait"
  echo "${label} start=${reading} waited=${waited}s at=$(date -Is)" >> "${GATES}"
  return 0
}

# ------------------------------------------------------------ ps liveness ----

# "measurement is running" is only worth reporting if the processes exist. The
# watcher samples the client and the servers while the runner is in flight and
# writes what it saw, so the claim is backed by a recording rather than by the
# absence of an error.
watch_processes () {
  local label="$1" pattern="$2" runner_pid="$3" out="${SPAN_DIR}/${label}.ps.txt"
  local samples=0 seen=0
  while kill -0 "${runner_pid}" 2>/dev/null; do
    if ps -eo pid,etime,pcpu,comm,args | grep -E "${pattern}" | grep -v grep > /tmp/.zl_ps.$$ 2>/dev/null; then
      seen=$((seen + 1))
      {
        echo "--- sample $((samples + 1)) at $(date -Is)"
        cat /tmp/.zl_ps.$$
      } >> "${out}"
    fi
    samples=$((samples + 1))
    sleep 10
  done
  rm -f /tmp/.zl_ps.$$
  note "ps watch ${label}: ${seen}/${samples} samples saw a matching process (${out})"
}

# ------------------------------------------------------------------ runners --

run_language () {
  local label="$1" pattern="$2"; shift 2
  wait_for_load "${label}" || return 1
  note "run ${label} begin"
  if [[ "${DRY_RUN}" == "1" ]]; then
    note "DRY_RUN ${label}: would exec: $*"
    ( sleep 2 ) &
    local stub=$!
    watch_processes "${label}" "${pattern}" "${stub}"
    wait "${stub}"
    note "run ${label} end rc=0 (dry run)"
    return 0
  fi
  "$@" > "${SPAN_DIR}/${label}.stdout" 2> "${SPAN_DIR}/${label}.stderr" &
  local runner=$!
  watch_processes "${label}" "${pattern}" "${runner}"
  wait "${runner}"
  local rc=$?
  note "run ${label} end rc=${rc} loadavg=$(cat /proc/loadavg)"
  return ${rc}
}

# ---------------------------------------------------------------------- main --

main () {
  note "verification span ${STAMP} begin  dry_run=${DRY_RUN}"
  note "commit=$(git -C "${REPO}" rev-parse HEAD)  branch=$(git -C "${REPO}" rev-parse --abbrev-ref HEAD)"
  note "loadavg at span begin: $(cat /proc/loadavg)"
  note "kernel=$(uname -r)"

  if ! preflight_ports; then
    note "ABORT: a bench port band is occupied; not relocating (spec 9)"
    return 2
  fi
  note "preflight: all five port bands free"

  local rc=0

  # .NET. Its runner has no load gate of its own, so the gate above is the only
  # one. MSBUILDDISABLENODEREUSE keeps an MSBuild daemon from inheriting the
  # lock file descriptor and holding the lock past the run.
  run_language dotnet 'WithGrpcBench' \
    env MSBUILDDISABLENODEREUSE=1 SKIP_BUILD=1 \
        OUTPUT="${DOTNET_DIR}/log/verify-${STAMP}/dotnet-router-1" \
        REPORT_FILE=report.txt RAW_SOCKET=router \
        "${DOTNET_DIR}/run_local.sh" || rc=1

  # Node. Its runner always appends one DEALER run after the ROUTER runs; that
  # extra run is recorded but the comparison uses node-router-1.
  run_language node 'node (client|grpc-server|zlink-raw-server|zlink-framework-server)/main.js' \
    env RUNS=1 STAMP="verify-${STAMP}" OUTROOT="${NODE_DIR}/log/verify-${STAMP}" \
        "${NODE_DIR}/run_local.sh" || rc=1

  run_language java 'bench-(client|grpc-server|zlink-raw-server|zlink-framework-server)' \
    env RUNS=1 RUN_DEALER=0 SKIP_BUILD=1 STAMP="verify-${STAMP}" \
        OUTROOT="${JAVA_DIR}/log/verify-${STAMP}" \
        "${JAVA_DIR}/run_local.sh" || rc=1

  run_language kotlin 'bench-(kotlin-client|grpc-server|zlink-raw-server|zlink-framework-server)' \
    env RUNS=1 RUN_DEALER=0 SKIP_BUILD=1 STAMP="verify-${STAMP}" \
        OUTROOT="${JAVA_DIR}/log/verify-${STAMP}" \
        "${JAVA_DIR}/run_local_kotlin.sh" || rc=1

  run_language cpp 'bench_cpp_(client|grpc_server|zlink_server)' \
    env RUN_STAMP="verify-${STAMP}" LOAD_GATE="${LOAD_GATE}" \
        "${CPP_DIR}/run_local.sh" cpp-router-1 \
        --implementations grpc-cpp,zlink-cpp \
        --duration-seconds 5 --warmup 5 --raw-socket router || rc=1

  # The C baseline is not a sixth language. It is the shared denominator of
  # formula 1, and both published judgements divide by it, so a reading of it
  # inside this span is what makes the two judgements comparable to the rest.
  run_language cbase 'bench_c_with_grpc_(zlink|grpc)_(client|server)' \
    env SKIP_BUILD=1 \
        OUTPUT="${C_DIR}/log/verify-${STAMP}/c-router-1" REPORT_FILE=report.txt \
        "${C_DIR}/run_local.sh" || rc=1

  note "loadavg at span end: $(cat /proc/loadavg)"
  note "verification span ${STAMP} end rc=${rc}"
  return ${rc}
}

# The whole span holds the lock, not each run separately -- otherwise another
# job could interleave between two languages and the runs would no longer share
# machine conditions, which is the one thing this span exists to establish.
if [[ "${ZLINK_PERF_LOCK_HELD:-0}" != "1" ]]; then
  export ZLINK_PERF_LOCK_HELD=1
  exec flock --exclusive --timeout 7200 /tmp/zlink-perf.lock "$0" "$@"
fi

main "$@"
