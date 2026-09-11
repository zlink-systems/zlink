#!/usr/bin/env bash
# Run the three server-driven C++ repetitions. Submit this script through
# scripts/perf/perf-ticket.sh; the queue owns the exclusive perf lock.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
export RUN_STAMP="${STAMP}"
SPAN_DIR="${SCRIPT_DIR}/../log/cpp/${STAMP}"
TIMELINE="${SPAN_DIR}/timeline.txt"

RUNS="${RUNS:-3}"
DURATION="${DURATION:-5}"
WARMUP="${WARMUP:-5}"
IMPLEMENTATIONS="${IMPLEMENTATIONS:-grpc-cpp,zlink-cpp,zlink-framework-cpp}"
PATTERNS="${PATTERNS:-request-serial,request-backpressure,send-saturation}"
PAYLOAD_SIZES="${PAYLOAD_SIZES:-1024,4096}"
LOAD_GATE="${LOAD_GATE:-2.0}"
export LOAD_GATE

[[ "${RUNS}" =~ ^[1-9][0-9]*$ ]] || { echo "RUNS must be a positive integer" >&2; exit 2; }
[[ "${DURATION}" =~ ^[1-9][0-9]*$ ]] || { echo "DURATION must be a positive integer" >&2; exit 2; }
[[ "${WARMUP}" =~ ^[1-9][0-9]*$ ]] || { echo "WARMUP must be a positive integer" >&2; exit 2; }

mkdir -p "${SPAN_DIR}"
: >"${TIMELINE}"

note() { printf '%s %s\n' "$(date -Is)" "$*" | tee -a "${TIMELINE}"; }

wait_for_load() {
  local waited=0 load
  while :; do
    load="$(awk '{print $1}' /proc/loadavg)"
    if awk -v measured_load="${load}" -v gate="${LOAD_GATE}" 'BEGIN { exit !(measured_load < gate) }'; then
      return 0
    fi
    if ((waited >= 600)); then
      note "load never fell below ${LOAD_GATE} within 600s (now ${load})"
      return 1
    fi
    sleep 10
    waited=$((waited + 10))
  done
}

run_one() {
  local run="$1" label="cpp-s2s-${run}"
  wait_for_load
  note "start ${label} loadavg=$(cat /proc/loadavg)"
  local rc
  if "${SCRIPT_DIR}/run_local.sh" "${label}" \
      --implementations "${IMPLEMENTATIONS}" \
      --patterns "${PATTERNS}" \
      --payload-sizes "${PAYLOAD_SIZES}" \
      --duration-seconds "${DURATION}" \
      --warmup "${WARMUP}"; then
    rc=0
  else
    rc=$?
  fi
  note "end ${label} rc=${rc} loadavg=$(cat /proc/loadavg)"
  return "${rc}"
}

note "span ${STAMP} begin: runs=${RUNS} duration=${DURATION}s warmup=${WARMUP}s"
note "commit=$(git -C "${SCRIPT_DIR}" rev-parse HEAD 2>/dev/null)"
for run in $(seq 1 "${RUNS}"); do
  run_one "${run}"
done
note "span ${STAMP} end rc=0"
