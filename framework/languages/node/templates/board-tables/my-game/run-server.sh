#!/usr/bin/env bash
set -euo pipefail
umask 077
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"
mkdir -p .logs
if ! (npm ci && npm run build) >.logs/build.log 2>&1; then
  tail -n 20 .logs/build.log >&2
  exit 1
fi
exec node dist/e2e/server-host.js
