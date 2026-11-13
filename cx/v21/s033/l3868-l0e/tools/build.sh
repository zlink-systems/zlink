#!/usr/bin/env bash
set -euo pipefail
ulimit -v 8388608
export VCPKG_MAX_CONCURRENCY=3
/root/jobs/fx/cx/cpp-build-slot.sh /root/jobs/fx/cx/v21/s033/l3868-l0e/tools/configure-build-inner.sh
