# shellcheck shell=bash
runner="$ROOT/e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll"

server_running() {
  [[ -f "$ROOT/.logs/server.json" && -f "$runner" ]] &&
    dotnet "$runner" --root "$ROOT" --is-running
}

build_local() {
  if server_running; then
    echo 'Stop the running server with Ctrl+C before building locally.' >&2
    exit 1
  fi
  mkdir -p "$ROOT/.logs"
  if ! dotnet build "$ROOT/server/MyGame.sln" -m:4 --nologo --verbosity minimal >"$ROOT/.logs/build.log" 2>&1; then
    tail -n 20 "$ROOT/.logs/build.log" >&2
    exit 1
  fi
}
