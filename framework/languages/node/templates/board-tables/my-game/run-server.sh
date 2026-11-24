#!/usr/bin/env bash
set -euo pipefail
umask 077
cd "$(dirname "${BASH_SOURCE[0]}")"
. ./scripts/run-helpers.sh

if [[ $# -ne 0 ]]; then
  echo 'Usage: ./run-server.sh' >&2
  exit 1
fi
mkdir -p .logs
assert_no_running_server
build_local
exec node dist/e2e/runner.js --serve
