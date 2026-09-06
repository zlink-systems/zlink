#!/usr/bin/env bash
# One measured span for the C++ with-grpc bench: three ROUTER runs then one
# DEALER run, all under a single exclusive /tmp/zlink-perf.lock.
#
# plan 3.2 / G7: this script builds nothing. Build before taking the lock.
# The load gate is re-checked before EVERY run, not once for the span, and each
# reading is appended to log/<stamp>/load-gates.txt.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
export RUN_STAMP="${STAMP}"
SPAN_DIR="${SCRIPT_DIR}/log/${STAMP}"
mkdir -p "${SPAN_DIR}"
TIMELINE="${SPAN_DIR}/timeline.txt"

DURATION="${DURATION:-5}"
WARMUP="${WARMUP:-5}"
IMPLEMENTATIONS="${IMPLEMENTATIONS:-grpc-cpp,zlink-cpp}"
LOAD_GATE="${LOAD_GATE:-2.0}"
export LOAD_GATE

note () { printf '%s %s\n' "$(date -Is)" "$*" | tee -a "${TIMELINE}"; }

wait_for_load () {
  local waited=0
  while ! awk -v g="${LOAD_GATE}" '{exit !($1 < g)}' /proc/loadavg; do
    if [[ ${waited} -ge 600 ]]; then
      note "load never fell below ${LOAD_GATE} within 600s (now $(awk '{print $1}' /proc/loadavg))"
      return 1
    fi
    sleep 10
    waited=$((waited + 10))
  done
  return 0
}

run_one () {
  local label="$1"; shift
  wait_for_load || return 1
  note "start ${label} loadavg=$(cat /proc/loadavg)"
  "${SCRIPT_DIR}/run_local.sh" "${label}" \
    --implementations "${IMPLEMENTATIONS}" \
    --duration-seconds "${DURATION}" \
    --warmup "${WARMUP}" \
    "$@"
  local rc=$?
  note "end   ${label} rc=${rc} loadavg=$(cat /proc/loadavg)"
  # Servers are restarted per run by run_local.sh, so one run's residue cannot
  # carry into the next.
  sleep 3
  return ${rc}
}

main () {
  note "span ${STAMP} begin: 3 ROUTER + 1 DEALER, duration=${DURATION}s warmup=${WARMUP}s"
  note "commit=$(git -C "${SCRIPT_DIR}" rev-parse HEAD 2>/dev/null)"
  local rc=0
  run_one cpp-router-1 --raw-socket router || rc=1
  run_one cpp-router-2 --raw-socket router || rc=1
  run_one cpp-router-3 --raw-socket router || rc=1
  # DEALER last (FB-001): the legacy configuration is measured for comparison and
  # is never the configuration a spec 7.2 judgement is taken from.
  run_one cpp-dealer-1 --raw-socket dealer || rc=1
  note "span ${STAMP} end rc=${rc}"
  return ${rc}
}

# Re-exec under the exclusive perf lock so the whole span -- all four runs --
# holds it, rather than each run racing for it separately.
if [[ "${ZLINK_PERF_LOCK_HELD:-0}" != "1" ]]; then
  export ZLINK_PERF_LOCK_HELD=1
  exec flock --exclusive --timeout 3600 /tmp/zlink-perf.lock "$0" "$@"
fi

main "$@"
