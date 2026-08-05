#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${NODE_ROOT}"
npm run build >/dev/null
node cross-language/node_dotnet_smoke.js
