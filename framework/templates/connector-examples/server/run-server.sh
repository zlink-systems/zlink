#!/usr/bin/env bash
set -euo pipefail
umask 077
cd "$(dirname "${BASH_SOURCE[0]}")"
if [[ $# -ne 0 ]]; then
  echo 'Usage: ./run-server.sh' >&2
  exit 1
fi
npm ci
npm run build
exec node --require ./dist/tracing.js dist/server.js
