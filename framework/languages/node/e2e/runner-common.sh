#!/usr/bin/env bash

ordered_e2e_roles() {
  local mode="$1"
  shift
  python3 - "$mode" "$@" <<'PY'
import random
import sys

mode = sys.argv[1]
roles = sys.argv[2:]
if mode in ("", "forward"):
    pass
elif mode == "reverse":
    roles.reverse()
elif mode.startswith("shuffle:"):
    seed_text = mode.split(":", 1)[1]
    if seed_text == "":
        raise SystemExit("E2E_START_ORDER shuffle requires a seed")
    random.Random(int(seed_text)).shuffle(roles)
else:
    raise SystemExit(f"unsupported E2E_START_ORDER={mode!r}")
for role in roles:
    print(role)
PY
}

pick_port() {
  node "$NODE_ROOT/e2e/port-picker.js"
}

allocate_port() {
  local port
  local registry="${PORT_REGISTRY_FILE:-${LOG_DIR:-/tmp/zlink-e2e-ports-$$}.allocated-ports}"
  while true; do
    port="$(pick_port)"
    if [[ ! -f "$registry" ]] || ! grep -Fxq "$port" "$registry"; then
      printf '%s\n' "$port" >>"$registry"
      printf '%s\n' "$port"
      return 0
    fi
  done
}

build_package() {
  local dir="$1"
  rm -rf "$dir/dist"
  (cd "$dir" && npm run build >/dev/null)
}

wait_health() {
  local url="$1"
  local name="$2"
  local pid="${3:-}"
  for _ in $(seq 1 "${LOCAL_READINESS_ATTEMPTS}"); do
    if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
      set +e
      wait "$pid"
      local code=$?
      set -e
      echo "$name exited before readiness at $url (exit=$code)" >&2
      return 1
    fi
    if curl --max-time "${HTTP_PROBE_TIMEOUT_SECONDS}" -fsS "$url/health" >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for $name at $url" >&2
  return 1
}

wait_tcp() {
  local name="$1"
  local endpoint="$2"
  local attempts="${3:-100}"
  local address="${endpoint#*://}"
  local host="${address%:*}"
  local port="${address##*:}"
  for _ in $(seq 1 "$attempts"); do
    if node -e "const net=require('node:net'); const s=net.connect({host: process.argv[1], port: Number(process.argv[2])}, () => { s.end(); process.exit(0); }); s.on('error', () => process.exit(1)); setTimeout(() => process.exit(1), 500);" "$host" "$port" >/dev/null 2>&1; then
      return 0
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "Timed out waiting for $name at $endpoint" >&2
  return 1
}

wait_port() {
  wait_tcp "$1" "$2" "${3:-$LOCAL_READINESS_ATTEMPTS}"
}

start_server() {
  local name="$1"
  local main="$2"
  shift
  shift
  if [[ "${ZLINK_E2E_RID_FROM_NAME:-0}" == "1" ]]; then
    ZLINK_E2E_RID="$name" node "$main" "$@" >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  else
    node "$main" "$@" >"$LOG_DIR/$name.stdout.log" 2>"$LOG_DIR/$name.stderr.log" &
  fi
  local pid="$!"
  LAST_STARTED_PID="$pid"
  pids+=("$pid")
  if declare -p pid_names >/dev/null 2>&1; then
    pid_names+=("$name")
  fi
  if declare -p pid_by_name >/dev/null 2>&1; then
    pid_by_name["$name"]="$pid"
  fi
}

stop_live_pids() {
  local pid
  for pid in "${pids[@]:-}"; do
    if kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
    fi
  done
}

wait_all_pids_ignoring_status() {
  wait "${pids[@]:-}" 2>/dev/null || true
}

remove_redis_container() {
  if [[ -n "${REDIS_CONTAINER_ID:-}" ]]; then
    docker rm -fv "$REDIS_CONTAINER_ID" >/dev/null 2>&1 || true
  fi
}

tail_failure_logs() {
  echo "E2E failed. log_dir=$LOG_DIR" >&2
  for file in "$LOG_DIR"/*.stderr.log "$LOG_DIR"/client.stderr.log; do
    if [[ -f "$file" ]]; then
      echo "----- $file -----" >&2
      tail -n 80 "$file" >&2 || true
    fi
  done
}

wait_file_contains() {
  local file="$1"
  local pattern="$2"
  local failure="$3"
  local pid="${4:-}"
  local attempts="${5:-$((SCENARIO_SETTLE_TIMEOUT_SECONDS * 10))}"
  for _ in $(seq 1 "$attempts"); do
    if [[ -f "$file" ]] && grep -F "$pattern" "$file" >/dev/null 2>&1; then
      return 0
    fi
    if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
      echo "$failure" >&2
      echo "process exited before marker: $pattern" >&2
      return 1
    fi
    sleep "${LOCAL_READINESS_POLL_SECONDS}"
  done
  echo "$failure" >&2
  echo "missing marker: $pattern" >&2
  return 1
}
