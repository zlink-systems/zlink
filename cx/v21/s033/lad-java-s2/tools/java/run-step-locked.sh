#!/usr/bin/env bash
set -euo pipefail
JOB=/root/jobs/fx/cx/v21/s033/lad-java-s2
ROOT=/root/worktree/zlink-v21-s033/lad-java
TOOLS="$JOB/tools/java"
RAW="$JOB/raw/java"
STEP="${1:?step required}"
MODE="${2:-initial}"
case "$STEP" in L0|L1|L2|L3|L4|L5_7) ;; *) exit 2;; esac
case "$MODE" in initial|repeat) ;; *) exit 2;; esac
cd "$ROOT"
client_jar="$ROOT/framework/bench/grpc/java/client/build/install/bench-client/lib/client.jar"
server_jar="$ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/lib/zlink-framework-server.jar"
raw_jar="$ROOT/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-raw-server.jar"
client_src="$ROOT/framework/bench/grpc/java/client/src"
shared_src="$ROOT/framework/bench/grpc/java/shared/src"
fw_src="$ROOT/framework/bench/grpc/java/zlink-framework-server/src"
for art in "$client_jar" "$server_jar" "$raw_jar"; do [[ -f "$art" ]] || { echo "missing artifact $art" >&2; exit 1; }; done
for src in "$client_src" "$shared_src" "$fw_src"; do
  while IFS= read -r f; do [[ "$client_jar" -nt "$f" || "$server_jar" -nt "$f" ]] || { echo "stale artifact for $f" >&2; exit 1; }; done < <(find "$src" -type f -name '*.java')
done
commit=$(git rev-parse HEAD); dirty=clean; [[ -z "$(git status --porcelain)" ]] || dirty=dirty
base=$(git merge-base HEAD origin/main)
artifacts="$(sha256sum "$client_jar" "$server_jar" "$raw_jar" "$ROOT/framework/bench/grpc/java/client/build/install/bench-client/bin/bench-client" "$ROOT/framework/bench/grpc/java/zlink-framework-server/build/install/bench-zlink-framework-server/bin/bench-zlink-framework-server" | awk '{printf "%s=%s;",$2,$1}')"
export SKIP_BUILD=1 RUNS=1 IMPLEMENTATIONS=zlink-framework-java
export WARMUP_SECONDS=10 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13
export SUBMIT_CONCURRENCY=8 WINDOW=100 SCHEDULING_RECORD=1
unset ZLINK_BENCH_LADDER_L0 ZLINK_BENCH_LADDER_L1 ZLINK_BENCH_LADDER_L2 ZLINK_BENCH_LADDER_L3 ZLINK_BENCH_LADDER_L4 ZLINK_BENCH_LADDER_L5_7
case "$STEP" in
  L0) export ZLINK_BENCH_LADDER_L0=1; receive='Framework server ZLinkFrameworkBenchServer.java:151-177'; reply='ZLinkFrameworkBenchServer.java:211-243'; extra='L0 fixed direct handler call; binding ZLinkRawBenchServer.java:94-109 receive, :115-127 reply; one blocking recv/message and same protobuf/body writes, one metrics record, no added atomics/copies/waits' ;;
  L1) export ZLINK_BENCH_LADDER_L1=1; receive='ZLinkFrameworkBenchServer.java:136-177'; reply='ZLinkFrameworkBenchServer.java:211-243'; extra='L1 adds application job permit allocation/acquire/transfer/return; binding ZLinkRawBenchServer.java:94-109 and :115-127; one blocking recv/message both, same protobuf/body writes, one metrics record, one added permit allocation and no added atomics/copies' ;;
  L2) export ZLINK_BENCH_LADDER_L2=1; receive='ZLinkFrameworkBenchServer.java:211-237'; reply='ZLinkFrameworkBenchServer.java:249-269'; extra='L2 request/response envelope encode and one header parse each direction; binding ZLinkRawBenchServer.java:94-109 and :115-127; same one blocking recv/message, added envelope allocations/parses, same metrics atomics and payload copies' ;;
  L3) export ZLINK_BENCH_LADDER_L3=1; receive='ZLinkFrameworkBenchServer.java:351-419'; reply='ZLinkFrameworkBenchServer.java:400-419'; extra='L3 service-wire multipart framing; binding ZLinkRawBenchServer.java:94-109 and :115-127; same one blocking recv/message, frame allocations/copies/unpack, same metrics atomics' ;;
  L4) export ZLINK_BENCH_LADDER_L4=1; receive='ZLinkFrameworkBenchServer.java:272-316'; reply='ZLinkFrameworkBenchServer.java:291-337'; extra='L4 typed JSON payload encode/decode; binding ZLinkRawBenchServer.java:94-109 and :115-127; same one blocking recv/message and metrics atomics, typed serialization allocations and frame copy' ;;
  L5_7) export ZLINK_BENCH_LADDER_L5_7=1; receive='ZLinkFrameworkBenchServer.java:419-435'; reply='BenchTypedEchoHandler.java:13-27; ZLinkMeshApplicationDispatcher.java:674-701,795-805'; extra='L5~L7 owner queue/worker, handler activation/DI scope, operation registry/deadline/completion; binding ZLinkRawBenchServer.java:94-109 and :115-127; adds Framework queue/dispatch waits and allocations, metrics decisions remain Framework-owned, typed JSON reply creates new response envelope/body and submits two parts' ;;
esac
if [[ "$MODE" == initial ]]; then kinds=(warm counted); else kinds=(counted); fi
start=$SECONDS
mkdir -p "$RAW/lock-windows" "$RAW/run-env"
for pair in 'send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096'; do
  read -r pattern size <<< "$pair"
  for kind in "${kinds[@]}"; do
    (( SECONDS - start < 1500 )) || { echo '25-minute safety boundary reached; stop this hold' >&2; exit 124; }
    if [[ "$kind" == warm ]]; then stage=warm; else stage=counted; fi
    attempt=1
    while [[ -e "$RAW/$STEP/$pattern-$size/$stage/attempt$attempt" ]]; do attempt=$((attempt + 1)); done
    out="$RAW/$STEP/$pattern-$size/$stage/attempt$attempt"
    mkdir -p "$out"
    stamp="lad-java-s2-${STEP}-${pattern}-${size}-${stage}-a${attempt}-$(date -u +%Y%m%dT%H%M%SZ)"
    export OUTPUT_DIR="$out" RUN_STAMP="$stamp" PATTERNS="$pattern" PAYLOAD_SIZES="$size"
    printf -- '- %s %s %s-%s %s built{HEAD=%s dirty=%s merge_base=%s artifacts=%s}; parity{binding receive=ZLinkRawBenchServer.java:94-109 reply=:115-127 || %s receive=%s reply=%s}; costs{%s}; raw=%s\n' "$(date -u +%FT%TZ)" "$STEP" "$pattern" "$size" "$kind" "$commit" "$dirty" "$base" "$artifacts" "$STEP" "$receive" "$reply" "$extra" "$out" >> "$JOB/progress.md"
    bash "$TOOLS/run_local_binding.sh" >"$out/driver.log" 2>&1 || { tail -80 "$out/driver.log" >&2; exit 1; }
    result="$out/run1/zlink-framework-java-${pattern}-${size}/results.json"
    sched="$out/run1/zlink-framework-java-${pattern}-${size}/scheduling.txt"
    [[ -f "$result" && -f "$sched" ]] || { echo "missing result/scheduling: $result $sched" >&2; exit 1; }
    python3 - "$result" "$sched" <<'PY'
import json,pathlib,sys
r=pathlib.Path(sys.argv[1]); s=pathlib.Path(sys.argv[2]); c=json.loads(r.read_text())['cells'][0]; t=s.read_text()
assert c['errors']==c['server_errors']==c['target_stats']['errors']==0
assert c['submitted']==c['completed']==c['target_stats']['received']
assert 'scheduling_invalid=' not in t
assert all(f'cpu{cpu}.state{state}.disable_at_run_start=1' in t for cpu in (0,8) for state in (2,3))
for phase,expected in (('source_start','0-5'),('mid_active_source','0-5'),('target_start','8-13'),('mid_active_target','8-13')):
 policies=[line for line in t.splitlines() if line.startswith(phase) and 'current scheduling policy:' in line]
 affs=[line.rsplit('cpus=',1)[1] for line in t.splitlines() if line.startswith(phase+'_affinity ') and 'cpus=' in line]
 assert policies and all('current scheduling policy: SCHED_OTHER' in x for x in policies),(phase,policies)
 assert affs and all(x==expected for x in affs),(phase,affs)
print(c['throughput_per_second'])
PY
    rate=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["cells"][0]["throughput_per_second"])' "$result")
    printf '%s\n' "$result" > "$RAW/$STEP/$pattern-$size/$stage/valid-result-a${attempt}.path"
    printf -- '- result %s %s-%s %s msg/s=%s raw=%s scheduling=%s\n' "$STEP" "$pattern" "$size" "$kind" "$rate" "$result" "$sched" >> "$JOB/progress.md"
  done
done
