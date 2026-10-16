#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
npm install
npm run build
# The runner selects server/session/main.js and server/match/main.js from declared node roles.
node dist/e2e/runner.js "$@"
