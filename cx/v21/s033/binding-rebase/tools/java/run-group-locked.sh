#!/usr/bin/env bash
set -euo pipefail
LANG=java
IMPL=zlink-java
WARMUP=10
JOB=/root/jobs/fx/cx/v21/s033/binding-rebase
TOOLS="$JOB/tools/$LANG"
ROOT=/root/worktree/zlink-v21-s033/lad-java
PROGRESS="$JOB/progress.md"
RAW="$JOB/raw/$LANG"
mkdir -p "$RAW/lock-windows" "$RAW/run-env"
START=$SECONDS
setgov() { for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
record_window() { { date -u +%FT%TZ; uptime; for cpu in $(seq 0 15); do printf 'cpu%s.governor=' "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; } > "$1"; }
trap 'setgov powersave' EXIT
setgov performance
record_window "$RAW/lock-windows/before.txt"
ready=0
for _ in $(seq 1 60); do
  ready=1
  for cpu in 0 8; do for state in 2 3; do
    [[ "$(cat /sys/devices/system/cpu/cpu${cpu}/cpuidle/state${state}/disable 2>/dev/null || echo unavailable)" == 1 ]] || ready=0
  done; done
  [[ "$ready" == 1 ]] && break
  sleep 0.25
done
[[ "$ready" == 1 ]] || { echo 'quiet guard did not disable C2/C3 before measurement' >&2; exit 1; }
printf '%s guard_ready C2/C3 disable=1\n' "$(date -u +%FT%TZ)" > "$RAW/lock-windows/guard-ready.txt"
IFS='|' read -r -a ARTS <<< '/root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/client/build/install/bench-client/lib/client.jar|/root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-raw-server.jar'
IFS='|' read -r -a SRCS <<< '/root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/client/src|/root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/shared/src|/root/worktree/zlink-v21-s033/lad-java/framework/bench/grpc/java/zlink-raw-server/src'
previous_signature=''
BUILD_META=''
BUILD_SIGNATURE=''
snapshot() {
  local commit dirty base artifacts signature art src newer idx
  commit=$(git -C "$ROOT" rev-parse HEAD)
  dirty=clean; [[ -z "$(git -C "$ROOT" status --porcelain)" ]] || dirty=dirty
  base=$(git -C "$ROOT" merge-base HEAD origin/main)
  artifacts=''
  if [[ "$LANG" == node ]]; then
    artifacts="live-source=$(stat -c '%y' "${ARTS[0]}")#$(sha256sum "${ARTS[0]}" | cut -d' ' -f1);live-raw=$(stat -c '%y' "${ARTS[1]}")#$(sha256sum "${ARTS[1]}" | cut -d' ' -f1);"
  else
    for idx in "${!ARTS[@]}"; do
      art="${ARTS[$idx]}"; src="${SRCS[$idx]}"
      [[ -f "$art" ]] || { echo "missing artifact: $art" >&2; return 1; }
      newer=1
      if [[ -d "$src" ]]; then
        while IFS= read -r f; do [[ "$art" -nt "$f" ]] || newer=0; done < <(find "$src" -type f \( -name '*.java' -o -name '*.kt' -o -name '*.cs' -o -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.inc' -o -name '*.proto' \))
      elif [[ -f "$src" ]]; then [[ "$art" -nt "$src" ]] || newer=0; fi
      [[ "$newer" == 1 ]] || { echo "stale artifact: $art" >&2; return 1; }
      artifacts+="${art}@$(stat -c '%y' "$art")#$(sha256sum "$art" | cut -d' ' -f1);"
    done
  fi
  signature="$artifacts"
  if [[ -n "$previous_signature" && "$signature" != "$previous_signature" ]]; then
    printf '%s artifact_changed_between_runs previous=%s current=%s\n' "$(date -u +%FT%TZ)" "$previous_signature" "$signature" >> "$JOB/progress.md"
  fi
  previous_signature="$signature"
  BUILD_SIGNATURE="$signature"
  BUILD_META="$(date -u +%FT%TZ) HEAD=$commit dirty=$dirty merge_base=$base artifacts=$artifacts"
}
one_run() {
  local pattern="$1" size="$2" kind="$3" stage="$4" attempt=1 out result sched rate meta
  while :; do
    (( SECONDS - START < 570 )) || { echo 'nine-minute safety boundary reached; stop this hold' >&2; return 124; }
    stage_root="$RAW/${pattern}-${size}/${stage}"
    while [[ -e "$stage_root/attempt${attempt}" ]]; do attempt=$((attempt+1)); done
    out="$stage_root/attempt${attempt}"
    mkdir -p "$out"
    snapshot
    meta="$BUILD_META"
    printf -- '- %s %s-%s %s built{%s}; receive{binding=ZLinkRawBenchServer.java:94-109 recv(NONE); L0=ZLinkFrameworkBenchServer.java:126-177 receive/permit} reply{binding=ZLinkRawBenchServer.java:115-127; L0=ZLinkFrameworkBenchServer.java:215-219}; costs{per message binding: 1 target blocking recv, source admission/completion wait, source request and reply protobuf writes, one metrics record, reused Received; L0 adds permit acquire/transfer; metrics atomics and payload copies unchanged}; raw=%s\n' "$LANG" "$pattern" "$size" "$kind" "$meta" "$out" >> "$PROGRESS"
    RUN_STAMP="4350-binding-${LANG}-${pattern}-${size}-${stage}-$(date -u +%Y%m%dT%H%M%SZ)"
    "$TOOLS/run-one-binding.sh" "$pattern" "$size" "$out" "$RUN_STAMP" "$WARMUP"
    result="$out/run1/${IMPL}-${pattern}-${size}/results.json"
    sched="$out/run1/${IMPL}-${pattern}-${size}/scheduling.txt"
    [[ -f "$result" && -f "$sched" ]] || { echo "missing result/scheduling: $result $sched" >&2; return 1; }
    if grep -q '^scheduling_invalid=' "$sched"; then
      printf '%s invalid_scheduling result=%s scheduling=%s; retry=%s\n' "$(date -u +%FT%TZ)" "$result" "$sched" "$attempt" >> "$PROGRESS"
      attempt=$((attempt+1)); (( attempt <= 5 )) || return 2; continue
    fi
    python3 - "$result" "$sched" <<'CHECK'
import json, pathlib, sys
r=pathlib.Path(sys.argv[1]); s=pathlib.Path(sys.argv[2]); c=json.loads(r.read_text())['cells'][0]
t=s.read_text()
assert all(c.get(k,0)==0 for k in ('errors','server_errors')) and c['target_stats']['errors']==0
assert c['submitted']==c['completed']==c['target_stats']['received']
assert all(f'cpu{cpu}.state{state}.disable_at_run_start=1' in t for cpu in (0,8) for state in (2,3))
for phase in ('source_start','target_start','mid_active_source','mid_active_target'):
 assert f'{phase}_tid=' in t, f'missing scheduling snapshot {phase}: {s}'
policies=[line for line in t.splitlines() if 'current scheduling policy:' in line]
assert policies and all('current scheduling policy: SCHED_OTHER' in line for line in policies), f'non-OTHER thread: {s}'
for label,expected in (('source_start','0-5'),('mid_active_source','0-5'),('target_start','8-13'),('mid_active_target','8-13')):
 affinities=[line.rsplit('cpus=',1)[1] for line in t.splitlines() if line.startswith(label+'_affinity ') and 'cpus=' in line]
 assert affinities and all(value==expected for value in affinities), f'wrong CPU split for {label}: {affinities}'
assert 'scheduling_invalid=' not in t
print(c['throughput_per_second'])
CHECK
    rate=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["cells"][0]["throughput_per_second"])' "$result")
    printf '%s\n' "$result" > "$stage_root/valid-result.path"
    printf -- '- result %s %s-%s %s msg/s=%s raw=%s scheduling=%s\n' "$LANG" "$pattern" "$size" "$kind" "$rate" "$result" "$sched" >> "$PROGRESS"
    break
  done
}
for pair in 'send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096'; do
  read -r pattern size <<<"$pair"
  one_run "$pattern" "$size" warm warm
  one_run "$pattern" "$size" counted counted
done
record_window "$RAW/lock-windows/after.txt"
