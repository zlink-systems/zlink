#!/bin/bash
set -euo pipefail
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64
exec /root/jobs/fx/cx/cpp-build-slot.sh bash /root/jobs/fx/cx/v21/s033/s4190c/c-build-commands.sh
