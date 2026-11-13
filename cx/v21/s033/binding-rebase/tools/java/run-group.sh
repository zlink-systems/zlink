#!/usr/bin/env bash
set -euo pipefail
flock -x /root/jobs/fx/cx/measure.lock /root/jobs/fx/cx/v21/s033/binding-rebase/tools/java/run-group-locked.sh
