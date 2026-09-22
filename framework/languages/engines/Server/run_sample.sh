#!/usr/bin/env bash
set -euo pipefail

action="${1:-}"
run_dir=".run"

find_port() {
  python3 - "$1" "$2" <<'PY'
import socket
import sys

for port in range(int(sys.argv[1]), int(sys.argv[2]) + 1):
    with socket.socket() as candidate:
        try:
            candidate.bind(("127.0.0.1", port))
        except OSError:
            continue
        print(port)
        raise SystemExit(0)
raise SystemExit("no free port in requested range")
PY
}

stop_run() {
  if [[ -f "$run_dir/server.pid" ]]; then
    server_pid="$(cat "$run_dir/server.pid")"
    if [[ "$server_pid" =~ ^[0-9]+$ ]]; then
      kill "$server_pid" 2>/dev/null || true
    fi
  fi
  if [[ -f "$run_dir/redis.container" ]]; then
    redis_container="$(cat "$run_dir/redis.container")"
    if [[ "$redis_container" =~ ^[0-9a-f]+$ ]]; then
      timeout 15s docker rm -fv "$redis_container" >/dev/null 2>&1 || true
    fi
  fi
  rm -f "$run_dir/server.pid" "$run_dir/redis.container"
}

case "$action" in
  install)
    dotnet restore EngineLobby.csproj
    ;;
  build)
    dotnet build EngineLobby.csproj -c Release
    ;;
  run)
    mkdir -p "$run_dir"
    if [[ -f "$run_dir/server.pid" ]] && kill -0 "$(cat "$run_dir/server.pid")" 2>/dev/null; then
      echo "Engine Lobby is already running" >&2
      exit 1
    fi

    redis_port="$(find_port 22000 22099)"
    mesh_port="$(find_port 22100 22699)"
    stream_port="$(find_port 22700 23299)"
    http_port="$(find_port 23300 23999)"
    redis_name="zlink-redis-dotnet-engine-lobby-$$-$RANDOM"

    redis_container="$(
      timeout 15s docker create --name "$redis_name" --tmpfs /data \
        -p "127.0.0.1:${redis_port}:6379" redis:7.2-alpine
    )"
    printf '%s\n' "$redis_container" > "$run_dir/redis.container"
    trap stop_run ERR
    timeout 15s docker start "$redis_container" >/dev/null

    published="$(
      timeout 15s docker inspect -f '{{(index (index .NetworkSettings.Ports "6379/tcp") 0).HostPort}}' \
        "$redis_container"
    )"
    if [[ "$published" != "$redis_port" ]]; then
      echo "Redis published port mismatch: expected $redis_port, got $published" >&2
      exit 1
    fi

    for _ in $(seq 1 60); do
      if python3 - "$redis_port" <<'PY'
import socket
import sys
with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=1):
    pass
PY
      then
        break
      fi
      sleep 1
    done

    prefix="engine-lobby-$(date +%s)-$$"
    cat > "$run_dir/settings.json" <<JSON
{
  "RedisEndpoint": "127.0.0.1:${redis_port}",
  "RedisKeyPrefix": "${prefix}",
  "MeshEndpoint": "tcp://127.0.0.1:${mesh_port}",
  "StreamEndpoint": "ws://127.0.0.1:${stream_port}",
  "HttpEndpoint": "http://127.0.0.1:${http_port}"
}
JSON
    printf '%s\n' "$stream_port" > "$run_dir/stream.port"
    printf '%s\n' "$http_port" > "$run_dir/http.port"

    dotnet run --project EngineLobby.csproj -c Release --no-build -- \
      server "$run_dir/settings.json" > "$run_dir/server.log" 2> "$run_dir/server.err.log" &
    printf '%s\n' "$!" > "$run_dir/server.pid"

    ready=0
    for _ in $(seq 1 60); do
      if curl -sf "http://127.0.0.1:${http_port}/ready" >/dev/null; then
        ready=1
        break
      fi
      if ! kill -0 "$(cat "$run_dir/server.pid")" 2>/dev/null; then
        break
      fi
      sleep 1
    done
    if [[ "$ready" != 1 ]]; then
      echo "Engine Lobby did not become ready; see $run_dir/server.err.log" >&2
      exit 1
    fi
    trap - ERR
    echo "engine-lobby-server=ready"
    ;;
  verify)
    http_port="$(cat "$run_dir/http.port")"
    stream_port="$(cat "$run_dir/stream.port")"
    curl -sf "http://127.0.0.1:${http_port}/ready" | grep -q '"ready":true'
    dotnet run --project EngineLobby.csproj -c Release --no-build -- \
      probe "ws://127.0.0.1:${stream_port}"
    ;;
  stop)
    stop_run
    ;;
  *)
    echo "usage: $0 install|build|run|verify|stop" >&2
    exit 2
    ;;
esac
