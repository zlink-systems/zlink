#!/bin/bash
# usage: measure-lock.sh <command...>
# Timing measurements run alone on the dedicated cores 0-3, one at a time across all jobs.
exec flock -o /root/jobs/fx/cx/measure.lock taskset -c 0-3 "$@"
