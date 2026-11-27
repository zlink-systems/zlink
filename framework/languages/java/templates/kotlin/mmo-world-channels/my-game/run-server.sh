#!/usr/bin/env bash
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
. "$ROOT/scripts/run-helpers.sh"

if [[ $# -ne 0 ]]; then
  echo 'Usage: ./run-server.sh' >&2
  exit 1
fi
if server_running; then
  echo 'A server is already running. Stop it with Ctrl+C in its terminal before building.' >&2
  exit 1
fi
echo 'Preparing: building the server (the first run downloads Gradle and dependencies and can take a few minutes)...'
build_local
echo 'Build complete. Starting servers...'
java=$(local_java)
exec "$java" "${java_flags[@]}" -cp "$e2e_classpath" MyGame.e2e.ServerHost
