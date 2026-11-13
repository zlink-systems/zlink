#!/bin/bash
# usage: measure-shared.sh <command...>
# Hold measure.lock shared for a test or build, after letting any waiting measurement go first.
. /root/jobs/fx/cx/measure-gate.sh
measure_gate_pass
exec flock -s "$MEASURE_LOCK" "$@"
