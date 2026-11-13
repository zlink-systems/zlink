#!/usr/bin/env bash
set -euo pipefail
TOOLS=/root/jobs/fx/cx/v21/s033/l3868-s1/tools
# The timeout starts only after flock grants ownership. Child bench groups are
# also bounded by Python, so an aborted hold cannot leave measured processes.
REMAINING=$(python3 -c 'import datetime,time; print(max(1,int(datetime.datetime(2026,11,13,23,55,tzinfo=datetime.timezone.utc).timestamp()-time.time())))')
flock -w "$REMAINING" -x /root/jobs/fx/cx/measure.lock timeout --signal=TERM --kill-after=5s 1740s chrt --other 0 python3 "$TOOLS/hold.py" "$@"
