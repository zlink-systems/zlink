#!/usr/bin/env bash

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
if cell["pattern"] == "send-saturation":
    seconds = float(cell["trigger"]["durationMs"]) / 1000.0
    throughput = int(target["received"]) / seconds
    cell["throughput_per_second"] = throughput
    cell["bandwidth_mb_s"] = throughput * int(cell["payload_size"]) / 1_000_000.0
temporary = result_path + ".merge"
with open(temporary, "w", encoding="utf-8") as handle:
    json.dump(result, handle, indent=2)
    handle.write("\n")
os.replace(temporary, result_path)
PY
}

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

emit_final_results() {
  python3 - "$1" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as handle:
    cell = json.load(handle)["cells"][0]
fields = {
    "throughput": "throughput_per_second", "bandwidth": "bandwidth_mb_s",
    "latency": "latency_mean_ms", "latency_p95": "latency_p95_ms",
    "latency_p99": "latency_p99_ms", "client_cpu_percent": "client_cpu_percent",
    "client_memory_mb": "client_memory_mb", "server_cpu_percent": "server_cpu_percent",
    "server_memory_mb": "server_memory_mb",
}
scenario = f'{cell["implementation"]}-{cell["pattern"]}'
for metric, field in fields.items():
    print(f'RESULT,current,{scenario},local,{cell["payload_size"]},{metric},{float(cell[field]):.3f}')
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
