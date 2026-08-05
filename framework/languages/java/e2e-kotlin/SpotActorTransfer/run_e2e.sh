#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAVA_SCENARIO_DIR="${ROOT_DIR}/../../e2e/SpotActorTransfer"

export ZLINK_JAVA_E2E_PROJECT_ROOT="${ROOT_DIR}"
export ZLINK_JAVA_E2E_LOG_ROOT="${ROOT_DIR}/log"
export ZLINK_JAVA_E2E_NODE_BIN="${ROOT_DIR}/Server/ActorNode/build/install/spot-actor-transfer-kotlin-actor-node/bin/spot-actor-transfer-kotlin-actor-node"
export ZLINK_JAVA_E2E_CLIENT_BIN="${ROOT_DIR}/Client/build/install/spot-actor-transfer-kotlin-client/bin/spot-actor-transfer-kotlin-client"

scenario="${1:-${ZLINK_JAVA_E2E_SCENARIO:-all}}"
if [[ "$#" -gt 0 ]]; then
  shift
fi
"${JAVA_SCENARIO_DIR}/run_e2e.sh" "$scenario" "$@"
