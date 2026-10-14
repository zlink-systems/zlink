#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
npm install
npm run build
# The runner selects server/session/main.js and server/channel/main.js and server/accounts/main.js from declared node roles.
node dist/e2e/runner.js "$@"
