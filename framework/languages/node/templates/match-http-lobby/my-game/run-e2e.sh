#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
npm install
npm run build
# The runner starts the separate match, Session, and HTTP lobby entrypoints.
node dist/e2e/runner.js "$@"
