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
  local started_ms deadline previous="" stable=0
  local stable_needed=$(((COMMAND_SETTLE_MS + 99) / 100))
  started_ms="$(date +%s%3N)"
  deadline=$((SECONDS + DRAIN_BOUND_MS / 1000))
  while ((SECONDS <= deadline)); do
    local source_body target_body counts
    source_body="$(curl --silent --show-error --fail "${source_url}/bench/stats")"
    target_body="$(curl --silent --show-error --fail "${target_url}/bench/stats")"
    counts="$(python3 -c \
      'import json,sys; a=json.loads(sys.argv[1]); b=json.loads(sys.argv[2]); print(a.get("completed",0),a.get("currentInFlight",0),b.get("received",0),b.get("errors",0))' \
      "${source_body}" "${target_body}")"
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
    sleep 0.1
  done
  curl --silent --show-error --fail "${target_url}/bench/stats" >"${target_file}"
  SETTLE_MS=$(($(date +%s%3N) - started_ms))
  SETTLE_BOUND_HIT=true
  return 1
}

# Adds B's counters and the drain result to the cell JSON A wrote. The aggregator
# (tools/bench_aggregate.py) recomputes send-saturation throughput from target_stats.
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
cell["target_stats"] = {
    "received": int(target["received"]),
    "errors": int(target["errors"]),
    "drainMs": float(drain_ms),
}
cell["drain_ms"] = float(drain_ms)
cell["drain_bound_hit"] = bound_hit == "true"
cell["server_received_at_close"] = int(target["received"])
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
errors = int(cell.get("errors", 0))
abandoned = int(cell.get("abandoned", 0))
received = int(cell["target_stats"]["received"])
if not completed <= received <= completed + errors + abandoned:
    raise SystemExit(
        f"request count mismatch: completed={completed} received={received} "
        f"errors={errors} abandoned={abandoned}")
if errors == 0 and abandoned == 0 and completed != received:
    raise SystemExit(f"request count mismatch: completed={completed} received={received}")
PY
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
