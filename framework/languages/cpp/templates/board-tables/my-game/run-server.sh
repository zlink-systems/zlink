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
build_local
exec "$runner" --root "$ROOT" --serve
