#!/bin/bash
# Holds measure.gate exclusive while an exclusive measure.lock taker is waiting (lslocks WRITE*)
# or holding it, and for a grace period after release: multi-stage measurements release the
# lock between stages, and shared takers must not slip in there. See measure-gate.sh.
. /root/jobs/fx/cx/measure-gate.sh
GRACE=${MEASURE_GATE_GRACE:-180}
exec 9>"$MEASURE_GATE"
exclusive() { lslocks -n -o MODE,PATH | awk -v l=$MEASURE_LOCK '$2==l && ($1=="WRITE*" || $1=="WRITE"){f=1} END{exit !f}'; }
while :; do
    if exclusive; then
        flock -x 9
        echo "$(date +%T) gate closed: measurement waiting or running"
        idle=0
        while [ $idle -lt $GRACE ]; do
            if exclusive; then idle=0; else idle=$((idle + 1)); fi
            sleep 1
        done
        flock -u 9
        echo "$(date +%T) gate open"
    fi
    sleep 1
done
