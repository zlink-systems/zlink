#!/bin/bash
# Writer preference for measure.lock (PM 2026-11-04). flock never lets a waiting exclusive
# taker go first, so overlapping shared takers can starve a measurement for hours.
# measure-gate-keeper.sh holds measure.gate exclusive while an exclusive waiter is blocked;
# new shared takers pass the gate first, so they wait until the measurement holds the lock.
# Source this file and call measure_gate_pass before `flock -s measure.lock`.
MEASURE_LOCK=/root/jobs/fx/cx/measure.lock
MEASURE_GATE=/root/jobs/fx/cx/measure.gate
measure_gate_pass() {
    # A caller that already holds measure.lock (nested wrapper) must not wait on the gate:
    # the waiting measurement is waiting for that caller.
    local holders p
    holders=" $(lslocks -n -o PID,PATH | awk -v l=$MEASURE_LOCK '$2==l{print $1}' | tr '\n' ' ') "
    p=$$
    while [ -n "$p" ] && [ "$p" -gt 1 ]; do
        [[ $holders == *" $p "* ]] && return 0
        p=$(awk '{print $4}' /proc/$p/stat 2>/dev/null)
    done
    flock -s "$MEASURE_GATE" true
}
# .NET build servers outlive the build and inherit the lock descriptor, holding measure.lock
# shared for many minutes after the job ends (2026-11-04: A/A waited behind 15 MSBuild nodes).
export MSBUILDDISABLENODEREUSE=1 DOTNET_CLI_USE_MSBUILD_SERVER=0 UseSharedCompilation=false
