#!/usr/bin/env bash
# Shared library for the six framework/bench/grpc runners (README §11).
# Runners `source` this file. It owns the common inputs (defaults, validation,
# OUTPUT_DIR resolution, cell directory layout) and the phase/settle/merge/verify helpers.

BENCH_GRPC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Values fixed by the specification (README §3). They are not inputs.
WINDOW=100
SEND_CONCURRENCY=8
TIMEOUT_SECONDS=300
COMMAND_SETTLE_MS=200
DRAIN_BOUND_MS=30000
REQUEST_TIMEOUT_MS=30000
ROUTE_READY_MS=30000
LATENCY_SAMPLE_LIMIT=200000
MEASUREMENT_LOAD_GATE=2.0
MEASUREMENT_LOAD_GATE_WAIT_SECONDS=600

bench_fail_input() {
  echo "$1" >&2
  exit 2
}

# bench_check_list <name> <csv> <allowed csv>
# Fails when <csv> is empty or has an entry outside <allowed csv>.
bench_check_list() {
  local name="$1" csv="$2" allowed=",$3," item
  local -a items
  [[ -n "${csv}" ]] || bench_fail_input "${name} must not be empty"
  IFS=',' read -r -a items <<<"${csv}"
  for item in "${items[@]}"; do
    [[ -n "${item}" && "${allowed}" == *",${item},"* ]] ||
      bench_fail_input "${name} entry '${item}' must be one of: $3"
  done
}

# bench_init <lang> <implementations csv> <allowed patterns csv> <allowed payloads csv>
#            [default patterns csv] [default payloads csv]
# Reads the README §11 environment variables, applies the defaults, and validates them.
# Exits 2 before any measurement when a value is out of range. Call it before the runner
# changes directory: a relative OUTPUT_DIR is resolved against the current directory.
# Sets: OUTPUT_DIR (absolute), RUN_STAMP, DURATION_SECONDS, RUNS, SKIP_BUILD, and the arrays
# implementations, patterns, payloads (in the order given).
bench_init() {
  local lang="$1" all_impls="$2" allowed_patterns="$3" allowed_payloads="$4"
  local default_patterns="${5:-$3}" default_payloads="${6:-$4}"

  RUN_STAMP="${RUN_STAMP:-$(date +%Y%m%d_%H%M%S)}"
  DURATION_SECONDS="${DURATION_SECONDS-5}"
  RUNS="${RUNS-3}"
  SKIP_BUILD="${SKIP_BUILD-0}"
  local payload_sizes="${PAYLOAD_SIZES-${default_payloads}}"
  local pattern_list="${PATTERNS-${default_patterns}}"
  local impl_list="${IMPLEMENTATIONS-${all_impls}}"

  [[ "${DURATION_SECONDS}" =~ ^[1-9][0-9]*$ ]] ||
    bench_fail_input "DURATION_SECONDS must be a positive integer"
  [[ "${RUNS}" =~ ^[1-9][0-9]*$ ]] || bench_fail_input "RUNS must be a positive integer"
  [[ "${SKIP_BUILD}" == 0 || "${SKIP_BUILD}" == 1 ]] || bench_fail_input "SKIP_BUILD must be 0 or 1"
  bench_check_list PAYLOAD_SIZES "${payload_sizes}" "${allowed_payloads}"
  bench_check_list PATTERNS "${pattern_list}" "${allowed_patterns}"
  bench_check_list IMPLEMENTATIONS "${impl_list}" "${all_impls}"
  IFS=',' read -r -a payloads <<<"${payload_sizes}"
  IFS=',' read -r -a patterns <<<"${pattern_list}"
  IFS=',' read -r -a implementations <<<"${impl_list}"

  OUTPUT_DIR="${OUTPUT_DIR:-${BENCH_GRPC_DIR}/log/${RUN_STAMP}/${lang}}"
  [[ "${OUTPUT_DIR}" == /* ]] || OUTPUT_DIR="${PWD}/${OUTPUT_DIR}"
  OUTPUT_DIR="${OUTPUT_DIR%/}"
}

# bench_cell_dir <run> <implementation> <pattern> <payload>
bench_cell_dir() {
  printf '%s/run%s/%s-%s-%s\n' "${OUTPUT_DIR}" "$1" "$2" "$3" "$4"
}

# bench_require_low_load: refuses to build while the machine is busy.
bench_require_low_load() {
  local load_average
  load_average="$(cut -d' ' -f1 /proc/loadavg)"
  awk -v value="${load_average}" 'BEGIN { exit !(value < 10.0) }' || {
    echo "load average must be below 10 before build (current ${load_average})" >&2
    return 1
  }
}

# Measurement gate shared by every runner. Build admission has a separate threshold.
bench_measurement_load_gate() {
  local load deadline=$((SECONDS + MEASUREMENT_LOAD_GATE_WAIT_SECONDS))
  while :; do
    load="$(awk '{print $1}' /proc/loadavg)"
    if awk -v measured_load="${load}" -v gate="${MEASUREMENT_LOAD_GATE}" \
      'BEGIN { exit !(measured_load < gate) }'; then
      return 0
    fi
    ((SECONDS < deadline)) || {
      echo "load average ${load} stayed above measurement gate ${MEASUREMENT_LOAD_GATE} for ${MEASUREMENT_LOAD_GATE_WAIT_SECONDS}s" >&2
      return 1
    }
    sleep 10
  done
}

select_java_home() {
  local candidate root
  local -a candidates=(
    "${HOME}/.cache/zlink/jdk/temurin-25"
    "/usr/lib/jvm/temurin-25-jdk-amd64"
    "/usr/lib/jvm/java-25-openjdk-amd64"
  )

  if [[ -n "${JAVA_HOME:-}" ]]; then
    if [[ -x "${JAVA_HOME}/bin/javac" ]]; then
      export JAVA_HOME
      return 0
    fi
    echo "JAVA_HOME is set to ${JAVA_HOME}, but ${JAVA_HOME}/bin/javac was not found or is not executable" >&2
    return 1
  fi

  for root in /usr/lib/jvm /usr/java /opt; do
    [[ -d "${root}" ]] || continue
    while IFS= read -r candidate; do
      candidates+=("${candidate}")
    done < <(find -L "${root}" -mindepth 1 -maxdepth 2 -type d -iname '*25*' -print 2>/dev/null | sort)
  done

  for candidate in "${candidates[@]}"; do
    if [[ -x "${candidate}/bin/javac" ]]; then
      export JAVA_HOME="${candidate}"
      return 0
    fi
  done

  echo "JAVA_HOME is not set; searched these JDK 25 candidates for executable bin/javac:" >&2
  for candidate in "${candidates[@]}"; do
    if [[ -e "${candidate}/bin/javac" ]]; then
      echo "  ${candidate}/bin/javac (present but not executable)" >&2
    else
      echo "  ${candidate}/bin/javac (not found)" >&2
    fi
  done
  return 1
}

check_ports_free() {
  local low="$1" high="$2" used
  used="$(ss -H -ltn | awk -v low="${low}" -v high="${high}" '
    { endpoint=$4; sub(/^.*:/, "", endpoint)
      if (endpoint ~ /^[0-9]+$/ && endpoint >= low && endpoint <= high) print $4 }')"
  if [[ -n "${used}" ]]; then
    echo "bench port range ${low}-${high} has active listeners: ${used}" >&2
    return 1
  fi
}

wait_for_ports_free() {
  local low="$1" high="$2" deadline=$((SECONDS + 30))
  while ((SECONDS < deadline)); do
    if check_ports_free "${low}" "${high}" 2>/dev/null; then return 0; fi
    sleep 0.1
  done
  check_ports_free "${low}" "${high}"
}

wait_for_stats() {
  local url="$1" require_ready="$2" deadline=$((SECONDS + 30)) body
  while ((SECONDS < deadline)); do
    if body="$(curl --silent --show-error --fail "${url}/bench/stats" 2>/dev/null)"; then
      if [[ "${require_ready}" != 1 ]] || python3 -c \
        'import json,sys; raise SystemExit(0 if json.load(sys.stdin).get("ready") is True else 1)' \
        <<<"${body}"; then
        return 0
      fi
    fi
    sleep 0.1
  done
  echo "stats endpoint did not become ready: ${url}" >&2
  return 1
}

wait_for_idle() {
  local url="$1" deadline=$((SECONDS + TIMEOUT_SECONDS)) body phase
  while ((SECONDS < deadline)); do
    body="$(curl --silent --show-error --fail "${url}/bench/stats")"
    phase="$(python3 -c 'import json,sys; print(json.load(sys.stdin).get("phase", ""))' \
      <<<"${body}")"
    case "${phase}" in
      idle) return 0 ;;
      failed) echo "source phase failed: ${body}" >&2; return 1 ;;
    esac
    sleep 0.1
  done
  echo "source phase did not complete: ${url}" >&2
  return 1
}

trigger_phase() {
  local url="$1" run_id="$2" cell_id="$3" pattern="$4" payload="$5" phase="$6" duration_ms="$7"
  curl --silent --show-error --fail -H 'content-type: application/json' \
    -d "{\"runId\":\"${run_id}\",\"cellId\":\"${cell_id}\",\"pattern\":\"${pattern}\",\"payloadBytes\":${payload},\"phase\":\"${phase}\",\"durationMs\":${duration_ms},\"requestWindow\":${WINDOW},\"sendConcurrency\":${SEND_CONCURRENCY}}" \
    "${url}/bench/start"
  echo
}

settle_and_capture() {
  local source_url="$1" target_url="$2" target_file="$3"
  local target_counter="${4:-received}" bound_ms="${5:-${DRAIN_BOUND_MS}}"
  local started_ms now_ms previous="" stable=0 source_body target_body counts
  local stable_needed=$(((COMMAND_SETTLE_MS + 99) / 100))
  started_ms="$(date +%s%3N)"
  while :; do
    source_body="$(curl --silent --show-error --fail --max-time 5 "${source_url}/bench/stats")"
    target_body="$(curl --silent --show-error --fail --max-time 5 "${target_url}/bench/stats")"
    counts="$(python3 -c '
import json,sys
a=json.loads(sys.argv[1]); b=json.loads(sys.argv[2])
received=b["anyPhaseMessages"] if sys.argv[3] == "any" else b["received"]
print(a.get("completed",0), a.get("currentInFlight", a.get("inFlight",0)),
      received, b.get("errors",0), b.get("rejected"))
' "${source_body}" "${target_body}" "${target_counter}")"
    if [[ "${counts}" == "${previous}" ]]; then
      stable=$((stable + 1))
      if ((stable >= stable_needed)); then
        printf '%s\n' "${target_body}" >"${target_file}"
        SETTLE_MS=$(($(date +%s%3N) - started_ms))
        SETTLE_BOUND_HIT=false
        return 0
      fi
    else
      previous="${counts}"
      stable=0
    fi
    now_ms="$(date +%s%3N)"
    if ((now_ms - started_ms >= bound_ms)); then break; fi
    sleep 0.1
  done
  curl --silent --show-error --fail --max-time 5 "${target_url}/bench/stats" >"${target_file}"
  SETTLE_MS=$(($(date +%s%3N) - started_ms))
  SETTLE_BOUND_HIT=true
}

# A owns the active-boundary receive count used for send throughput. B's later snapshot
# records settle counts separately.
merge_target_stats() {
  local result_file="$1" target_file="$2" drain_ms="$3" bound_hit="$4"
  python3 - "${result_file}" "${target_file}" "${drain_ms}" "${bound_hit}" <<'PY'
import json, os, sys
result_path, target_path, drain_ms, bound_hit = sys.argv[1:]
with open(result_path, encoding="utf-8") as handle:
    result = json.load(handle)
with open(target_path, encoding="utf-8") as handle:
    target = json.load(handle)
cell = result["cells"][0]
received = target["received"]
rejected = target.get("rejected")
cell["target_stats"] = {
    "received": int(received),
    "errors": int(target["errors"]),
}
if "rejected" in target:
    cell["target_stats"]["rejected"] = int(rejected) if rejected is not None else None
    cell["server_rejected_count"] = cell["target_stats"]["rejected"]
source_drain_ms = float(cell.get("source_drain_ms", cell.get("drain_ms", 0.0)))
if "source_drain_ms" in cell or "drain_ms" in cell:
    cell["source_drain_ms"] = source_drain_ms
    cell["runner_settle_ms"] = float(drain_ms)
cell["drain_ms"] = source_drain_ms + float(drain_ms)
cell["target_stats"]["drainMs"] = cell["drain_ms"]
cell["drain_bound_hit"] = bool(cell.get("drain_bound_hit", False)) or bound_hit == "true"
temporary = result_path + ".merge"
with open(temporary, "w", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2)
    handle.write("\n")
os.replace(temporary, result_path)
PY
}

# B may receive requests A never completed (abandoned after the active window, or failed at A
# after reaching B): completed <= received <= completed + errors + abandoned, with equality
# when A saw no error and abandoned nothing.
verify_request_counts() {
  python3 - "$1" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    cell = json.load(handle)["cells"][0]
completed = int(cell["completed"])
submitted = int(cell["submitted"]) if "submitted" in cell else None
errors = int(cell["errors"])
abandoned = int(cell["abandoned"])
received = int(cell["target_stats"]["received"])
target_errors = int(cell["target_stats"]["errors"])
target_reports_rejections = "rejected" in cell["target_stats"]
rejected = cell.get("server_rejected_count")
if target_reports_rejections and cell["pattern"] == "send-saturation" and rejected is None:
    raise SystemExit("target rejection count unavailable: completed == received + rejected cannot be verified")
if rejected is not None:
    rejected = int(rejected)
if submitted is not None and submitted != completed + errors + abandoned:
    raise SystemExit(f"source count mismatch: submitted={submitted} completed={completed} "
                     f"errors={errors} abandoned={abandoned}")
if target_errors != 0:
    raise SystemExit(f"target reported errors={target_errors}")
if cell["pattern"].startswith("request-") and not completed <= received <= completed + errors + abandoned:
    raise SystemExit(
        f"request count mismatch: completed={completed} received={received} "
        f"errors={errors} abandoned={abandoned}")
accounted = received + rejected if cell["pattern"] == "send-saturation" and target_reports_rejections else received
if (rejected is not None and rejected < 0) or (submitted is not None and accounted > submitted):
    raise SystemExit(f"target count mismatch: submitted={submitted} received={received} rejected={rejected}")
if errors == 0 and abandoned == 0 and completed != accounted:
    raise SystemExit(f"{cell['pattern']} count mismatch: completed={completed} "
                     f"received={received} rejected={rejected}")
print(f"counts: completed={completed} received={received} server_rejected_count={rejected} "
      f"difference={completed - received}")
PY
}

bench_record_contaminated_cell() {
  local result_file="$1" implementation="$2" pattern="$3" payload="$4" reason="$5"
  python3 - "${result_file}" "${implementation}" "${pattern}" "${payload}" "${reason}" <<'PY'
import json, sys
path, implementation, pattern, payload, reason = sys.argv[1:]
with open(path, "w", encoding="utf-8") as handle:
    cell = {
        "implementation": implementation, "pattern": pattern, "payload_size": int(payload),
        "contaminated": True, "contamination_reason": reason,
    }
    json.dump({"schema": "with-grpc-cell-v1", "cells": [cell]}, handle, indent=2)
    handle.write("\n")
PY
}

# One server-driven cell, from warmup through the B snapshot. The optional reset
# endpoint is for B implementations whose warmup counters need an explicit reset.
bench_run_cell() {
  local trigger_url="$1" source_url="$2" target_url="$3" run_id="$4" cell_id="$5"
  local implementation="$6" pattern="$7" payload="$8" warmup_ms="$9"
  local result_file="${10}" target_file="${11}" reset_url="${12:-}"
  local remaining_drain_ms source_bound_hit drain_state
  trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" warmup "${warmup_ms}"
  wait_for_idle "${source_url}"
  if [[ -n "${reset_url}" ]]; then
    settle_and_capture "${source_url}" "${target_url}" /dev/null any
  else
    settle_and_capture "${source_url}" "${target_url}" /dev/null
  fi
  if [[ "${SETTLE_BOUND_HIT}" == true ]]; then
    bench_record_contaminated_cell "${result_file}" "${implementation}" "${pattern}" "${payload}" \
      "warmup settle bound hit"
    echo "warmup settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id} (recorded; cell excluded)" >&2
    return 0
  fi
  if [[ -n "${reset_url}" ]]; then
    curl --silent --show-error --fail --max-time 5 -X POST "${reset_url}/bench/reset" >/dev/null
  fi
  trigger_phase "${trigger_url}" "${run_id}" "${cell_id}" "${pattern}" "${payload}" active "$((DURATION_SECONDS * 1000))"
  wait_for_idle "${source_url}"
  [[ -s "${result_file}" ]] || { echo "missing source result: ${result_file}" >&2; return 1; }
  drain_state="$(python3 - "${result_file}" "${DRAIN_BOUND_MS}" <<'PY'
import json, math, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    cell = json.load(handle)["cells"][0]
print(max(0, int(sys.argv[2]) - math.ceil(float(cell.get("source_drain_ms", cell.get("drain_ms", 0))))),
      str(bool(cell.get("drain_bound_hit", False))).lower())
PY
)" || return 1
  read -r remaining_drain_ms source_bound_hit <<<"${drain_state}"
  settle_and_capture "${source_url}" "${target_url}" "${target_file}" received "${remaining_drain_ms}"
  merge_target_stats "${result_file}" "${target_file}" "${SETTLE_MS}" "${SETTLE_BOUND_HIT}"
  if [[ "${SETTLE_BOUND_HIT}" == true || "${source_bound_hit}" == true ]]; then
    echo "cell settle hit ${DRAIN_BOUND_MS}ms bound: ${cell_id} (recorded; run continues)" >&2
    return 0
  fi
  verify_request_counts "${result_file}"
}

cleanup_cell() {
  local status=$?
  set +e
  for pid in "${a_pid:-}" "${b_pid:-}"; do
    [[ -n "${pid}" ]] || continue
    kill -TERM -- "-${pid}" >/dev/null 2>&1 || kill -TERM "${pid}" >/dev/null 2>&1 || true
  done
  local deadline=$((SECONDS + 3))
  while ((SECONDS < deadline)); do
    local running=0
    for pid in "${a_pid:-}" "${b_pid:-}"; do
      [[ -n "${pid}" ]] && kill -0 "${pid}" >/dev/null 2>&1 && running=1
    done
    ((running == 0)) && break
    sleep 0.1
  done
  for pid in "${a_pid:-}" "${b_pid:-}"; do
    [[ -n "${pid}" ]] || continue
    kill -KILL -- "-${pid}" >/dev/null 2>&1 || kill -KILL "${pid}" >/dev/null 2>&1 || true
    wait "${pid}" >/dev/null 2>&1 || true
  done
  a_pid=""
  b_pid=""
  set -e
  return "${status}"
}
