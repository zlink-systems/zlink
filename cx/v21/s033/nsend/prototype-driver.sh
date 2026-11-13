#!/usr/bin/env bash
set -euo pipefail
/root/jobs/fx/cx/measure-shared.sh bash /root/jobs/fx/cx/v21/s033/nsend/build-prototype.sh > /root/jobs/fx/cx/v21/s033/nsend/prototype-checks.log 2>&1
exec flock -o /root/jobs/fx/cx/measure.lock python3 /root/jobs/fx/cx/v21/s033/nsend/prototype-measure.py
