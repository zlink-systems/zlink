#!/usr/bin/env bash

NODE_E2E_REDIS_PORT_MIN=38000
NODE_E2E_REDIS_PORT_MAX=38099

redis_port_is_available() {
  python3 - "$1" <<'PY'
import socket
import sys

candidate = int(sys.argv[1])
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    sock.bind(("127.0.0.1", candidate))
except OSError:
    raise SystemExit(1)
finally:
    sock.close()
PY
}

pick_redis_host_port() {
  python3 - "$NODE_E2E_REDIS_PORT_MIN" "$NODE_E2E_REDIS_PORT_MAX" <<'PY'
import random
import sys

minimum = int(sys.argv[1])
maximum = int(sys.argv[2])
print(random.randint(minimum, maximum))
PY
}

wait_redis_ready() {
  local container_id="$1"
  local timeout_seconds="${ZLINK_REDIS_READY_TIMEOUT_SECONDS:-60}"
  local deadline=$((SECONDS + timeout_seconds))

  while (( SECONDS < deadline )); do
    if timeout -k 2s 5s docker exec "${container_id}" redis-cli ping 2>/dev/null | grep -q '^PONG$'; then
      return 0
    fi
    sleep 1
  done

  printf 'Timed out waiting for Redis container %s to answer PING\n' "${container_id}" >&2
  return 1
}

remove_redis_attempt() {
  local candidate="$1"
  local name="$2"
  if [[ ! "$candidate" =~ ^[0-9a-f]{12,64}$ ]]; then
    candidate="$(timeout -k 2s 5s docker inspect --type container \
      -f '{{.Id}}' "$name" 2>/dev/null || true)"
  fi
  if [[ "$candidate" =~ ^[0-9a-f]{12,64}$ ]]; then
    timeout -k 2s 10s docker rm -fv "$candidate" >/dev/null 2>&1 || true
  fi
}

start_redis_container() {
  local name="$1"
  local image="$2"
  local requested_port="${3:-}"
  local attempt selected_port attempt_name create_output create_status start_output start_status
  local candidate running published_port failure_details

  REDIS_CONTAINER_ID=""
  REDIS_HOST_PORT=""
  for attempt in $(seq 1 100); do
    selected_port="$requested_port"
    if [[ -z "$selected_port" ]]; then
      selected_port="$(pick_redis_host_port)"
    fi
    if ! redis_port_is_available "$selected_port"; then
      if [[ -n "$requested_port" ]]; then
        sleep 0.1
      fi
      continue
    fi
    attempt_name="${name}-${attempt}-${selected_port}"

    set +e
    create_output="$(timeout -k 2s 10s docker create --name "$attempt_name" --tmpfs /data \
      -p "127.0.0.1:${selected_port}:6379" "$image" 2>&1)"
    create_status="$?"
    set -e
    candidate="$(printf '%s\n' "$create_output" | awk '/^[0-9a-f]{12,64}$/ { print; exit }')"
    if [[ "$create_status" != "0" || -z "$candidate" ]]; then
      remove_redis_attempt "$candidate" "$attempt_name"
      if grep -Eqi 'port is already allocated|address already in use|bind.*failed' \
          <<<"$create_output"; then
        [[ -n "$requested_port" ]] && sleep 0.1
        continue
      fi
      printf 'Failed to create Redis container %s (docker status %s)\n%s\n' \
        "$attempt_name" "$create_status" "$create_output" >&2
      return 1
    fi

    set +e
    start_output="$(timeout -k 2s 10s docker start "$candidate" 2>&1)"
    start_status="$?"
    set -e
    running="$(timeout -k 2s 5s docker inspect -f '{{.State.Running}}' "$candidate" 2>/dev/null || true)"
    published_port="$(timeout -k 2s 5s docker inspect \
      -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' \
      "$candidate" 2>/dev/null || true)"
    if [[ "$running" == "true" && "$published_port" == "$selected_port" ]]; then
      REDIS_CONTAINER_ID="$candidate"
      REDIS_HOST_PORT="$selected_port"
      if wait_redis_ready "$candidate"; then
        return 0
      fi
      remove_redis_attempt "$candidate" "$attempt_name"
      REDIS_CONTAINER_ID=""
      REDIS_HOST_PORT=""
      return 1
    fi

    remove_redis_attempt "$candidate" "$attempt_name"
    failure_details="${start_output} running=${running} published=${published_port} expected=${selected_port}"
    if grep -Eqi 'port is already allocated|address already in use|bind.*failed' \
        <<<"$failure_details" \
        || [[ "$running" == "true" && "$published_port" != "$selected_port" ]]; then
      [[ -n "$requested_port" ]] && sleep 0.1
      continue
    fi
    printf 'Failed to start Redis container %s on host port %s (docker status %s)\n%s\n' \
      "$attempt_name" "$selected_port" "$start_status" "$failure_details" >&2
    return 1
  done

  if [[ -n "$requested_port" ]]; then
    printf 'Could not restart Redis on the requested Node.js E2E host port %s.\n' \
      "$requested_port" >&2
  else
    printf 'Could not start Redis in the Node.js E2E port range %s-%s.\n' \
      "$NODE_E2E_REDIS_PORT_MIN" "$NODE_E2E_REDIS_PORT_MAX" >&2
  fi
  return 1
}

redis_container_endpoint() {
  local container_id="$1"
  local timeout_seconds="${ZLINK_REDIS_READY_TIMEOUT_SECONDS:-60}"
  local deadline=$((SECONDS + timeout_seconds))
  local host_port

  while (( SECONDS < deadline )); do
    host_port="$(timeout -k 2s 5s docker inspect \
      -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' \
      "${container_id}" 2>/dev/null || true)"
    if [[ -n "${host_port}" ]] \
      && timeout 1 bash -c ":</dev/tcp/127.0.0.1/${host_port}" >/dev/null 2>&1; then
      printf '127.0.0.1:%s\n' "${host_port}"
      return 0
    fi
    sleep 0.1
  done

  printf 'Timed out waiting for Redis container %s host port to accept connections\n' "${container_id}" >&2
  return 1
}
