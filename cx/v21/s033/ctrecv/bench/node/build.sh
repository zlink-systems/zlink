#!/usr/bin/env bash
# Builds the Node with-grpc bench. Owner of the Node build command; run_local.sh and build_all.sh call it.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FRAMEWORK_DIR="$(cd "${HERE}/../../../languages/node" && pwd)"
# shellcheck source=../runner_common.sh
source "${HERE}/../runner_common.sh"
cd "${HERE}"

bench_require_low_load
node "${FRAMEWORK_DIR}/scripts/materialize-local-http-client-package.mjs"
npm --prefix "${FRAMEWORK_DIR}" ci
npm ci
npm run build
