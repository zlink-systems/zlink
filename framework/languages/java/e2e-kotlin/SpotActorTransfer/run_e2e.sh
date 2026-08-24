#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$(cd "${ROOT_DIR}/../.." && pwd)/e2e-runner-common.sh"
export ZLINK_E2E_RUNNER_LANGUAGE_OVERRIDE=kotlin
zlink_e2e_initialize kotlin "$0" "$@"
JAVA_SCENARIO_DIR="${ROOT_DIR}/../../e2e/SpotActorTransfer"

export ZLINK_JAVA_E2E_PROJECT_ROOT="${ROOT_DIR}"
export ZLINK_JAVA_E2E_LOG_ROOT="${ROOT_DIR}/log"
export ZLINK_JAVA_E2E_NODE_BIN="${ROOT_DIR}/Server/ActorNode/build/install/spot-actor-transfer-kotlin-actor-node/bin/spot-actor-transfer-kotlin-actor-node"
export ZLINK_JAVA_E2E_CLIENT_BIN="${ROOT_DIR}/Client/build/install/spot-actor-transfer-kotlin-client/bin/spot-actor-transfer-kotlin-client"

scenario="${1:-${ZLINK_JAVA_E2E_SCENARIO:-all}}"
# Inventory blockers: ST-A2 ST-A3 ST-B1 ST-B2 ST-B3 ST-B4 ST-C2 ST-C4 ST-D1
# ST-D2 ST-E1A ST-E1B ST-E1C ST-F1 ST-F2 ST-F3 ST-F3A ST-F5 ST-F6
# ST-G1 ST-G2 ST-G3 ST-G4 ST-G5 ST-G6 ST-H1 ST-H2 ST-H3 ST-H4 ST-H4A
# ST-H4B ST-H5 ST-I1 ST-I2 ST-I3 ST-I4 ST-I5 ST-I6. Component tests do
# not replace the missing process-level relocation gates.
if [[ "$#" -gt 0 ]]; then
  shift
fi
bash "${JAVA_SCENARIO_DIR}/run_e2e.sh" "$scenario" "$@"
