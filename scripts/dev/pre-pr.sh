#!/usr/bin/env bash
# Reuse the Linux x64 batch steps. Leave checkout, setup actions and toolchain
# installs, caches, artifact transport, Redis lifecycle, Windows/release/nightly
# steps, cross-platform result comparison and Pages deployment to CI.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
exec python3 scripts/dev/pre-pr.py "$@"
