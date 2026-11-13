#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/jl57bp
BENCH=/root/worktree/zlink-v21-s033/lad-java-l57p/framework/bench/grpc/java
printf 'fixed_hold_start_utc=%s\n' "$(date -u +%FT%TZ)" >> "$JOB/raw/fixed-hold.txt"
trap 'printf "fixed_hold_end_utc=%s\n" "$(date -u +%FT%TZ)" >> "$JOB/raw/fixed-hold.txt"' EXIT
chrt -o 0 taskset -c 0-5 /usr/lib/jvm/java-25-openjdk-amd64/bin/java \
 -cp "$BENCH/client/build/classes/java/test:$BENCH/client/build/install/bench-client/lib/*" \
 systems.zlink.bench.withgrpc.client.AdmissionProgressTest > "$JOB/raw/admission-progress-test.log" 2>&1
printf 'admissionProgressTest_exit=0 utc=%s\n' "$(date -u +%FT%TZ)" >> "$JOB/raw/admission-progress-test.log"
bash "$JOB/tools/run-pair-locked.sh" fixed-L5_7
