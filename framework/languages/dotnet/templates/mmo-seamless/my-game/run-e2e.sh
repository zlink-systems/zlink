#!/usr/bin/env bash
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
mkdir -p "$ROOT/.logs"
if [[ "${1:-}" == "--no-build" ]]; then
  shift
else
  if ! dotnet build "$ROOT/server/MyGame.sln" --nologo --verbosity minimal >"$ROOT/.logs/build.log" 2>&1; then
    tail -n 20 "$ROOT/.logs/build.log" >&2
    exit 1
  fi
fi
exec dotnet "$ROOT/e2e/MyGame.E2E/bin/Debug/net8.0/MyGame.E2E.dll" --root "$ROOT" "$@"
