#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
npm ci
npm run build
node dist/e2e/runner.js "$@"
