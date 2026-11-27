# shellcheck shell=bash
runner="$ROOT/e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll"

server_running() {
  [[ -f "$ROOT/.logs/server.json" && -f "$runner" ]] &&
    dotnet "$runner" --root "$ROOT" --is-running
}

build_local() {
  if server_running; then
    echo 'Stop the running server with Ctrl+C before building locally.' >&2
    return 1
  fi
  mkdir -p "$ROOT/.logs"
  local log
  log=$(mktemp -d "$ROOT/.logs/run-build-XXXXXXXX")/build.log
  if ! dotnet build "$ROOT/server/MyGame.sln" -m:4 --nologo --verbosity minimal >"$log" 2>&1; then
    tail -n 20 "$log" >&2
    echo "Server build failed. See $log." >&2
    return 1
  fi
}
