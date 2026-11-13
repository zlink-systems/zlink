#!/usr/bin/env bash
set -euo pipefail
exec flock -x /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/v21/s033/lad-dotnet-s1/tools/dotnet/run-group-locked.sh "$@"
