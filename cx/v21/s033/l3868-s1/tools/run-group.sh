#!/usr/bin/env bash
set -euo pipefail
TOOLS=/root/jobs/fx/cx/v21/s033/l3868-s1/tools
# The timeout starts only after flock grants ownership. Child bench groups are
# also bounded by Python, so an aborted hold cannot leave measured processes.
flock -x /root/jobs/fx/cx/measure.lock timeout --signal=TERM --kill-after=5s 1740s chrt --other 0 python3 "$TOOLS/hold.py" "$@"
