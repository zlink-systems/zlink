#!/usr/bin/env bash
set -euo pipefail
exec chrt -o 0 flock -x /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/v21/s033/lad-dotnet-s1/tools/dotnet/run-group-locked.sh "$@"
