#!/bin/bash
set -euo pipefail
case "${1:-}" in
 --build) exec /root/jobs/fx/cx/cpp-build-slot.sh /usr/bin/cmake "$@" --parallel 3 ;;
 --install|-E|-P|--version) exec /usr/bin/cmake "$@" ;;
 *) exec /root/jobs/fx/cx/cpp-build-slot.sh /usr/bin/cmake "$@" ;;
esac
