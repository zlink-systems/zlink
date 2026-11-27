# shellcheck shell=bash
runner="$ROOT/build/bin/my-game-e2e"

build_local() {
  mkdir -p "$ROOT/.logs"
  if ! { cmake -P bootstrap.cmake && cmake --preset default && cmake --build --preset default; } >"$ROOT/.logs/build.log" 2>&1; then
    tail -n 20 "$ROOT/.logs/build.log" >&2
    exit 1
  fi
}

server_running() {
  [[ -f "$ROOT/.logs/server.json" && -x "$runner" ]] && "$runner" --root "$ROOT" --server-running
}

assert_no_running_server() {
  if server_running; then
    echo 'A server is already running. Stop it with Ctrl+C in its terminal before building.' >&2
    exit 1
  fi
}
