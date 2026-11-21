#!/usr/bin/env bash
set -eo pipefail
# The workspace manifest pins http-client to a local tarball; build its
# dist with the same bootstrap the local-package flow uses, fed by the
# checkout-built local binding tarball.
package_root="${1}"
npm_root="${package_root}/npm"
mkdir -p "${npm_root}"
bash scripts/local-package/http-client/build-wsl.sh node
tar tzf "${npm_root}"/zlink-systems-http-client-*.tgz > "${RUNNER_TEMP}/http-client-files.txt"
grep -q 'package/dist/index.js' "${RUNNER_TEMP}/http-client-files.txt"
