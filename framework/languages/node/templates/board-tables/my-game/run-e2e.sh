#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
npm ci
npm run build
# The runner selects server/session/main.js and server/table/main.js from declared node roles.
node dist/e2e/runner.js "$@"
