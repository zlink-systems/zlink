#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NODE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${NODE_ROOT}"
npm run build >/dev/null
dotnet build "${NODE_ROOT}/../dotnet/cross-language/Zlink.Framework.TestHost/Zlink.Framework.TestHost.csproj" \
  --framework net8.0 >/dev/null
ZLINK_DOTNET_TESTHOST_NO_BUILD=1 node cross-language/node_dotnet_smoke.js
