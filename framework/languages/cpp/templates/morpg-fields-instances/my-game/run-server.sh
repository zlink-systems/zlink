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
assert_no_running_server
echo 'Preparing: building the server (the first run downloads the Framework prebuilt)...'
build_local
echo 'Build complete. Starting servers...'
exec "$runner" --root "$ROOT" --serve
