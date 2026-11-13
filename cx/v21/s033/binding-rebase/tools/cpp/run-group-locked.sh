#!/usr/bin/env bash
set -euo pipefail
LANG=cpp
IMPL=zlink-cpp
WARMUP=3
JOB=/root/jobs/fx/cx/v21/s033/binding-rebase
TOOLS="$JOB/tools/$LANG"
ROOT=/root/worktree/zlink-v21-s033/l3868
PROGRESS="$JOB/progress.md"
RAW="$JOB/raw/$LANG"
mkdir -p "$RAW/lock-windows" "$RAW/run-env"
START=$SECONDS
setgov() { for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
record_window() { { date -u +%FT%TZ; uptime; for cpu in $(seq 0 15); do printf 'cpu%s.governor=' "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; } > "$1"; }
trap 'setgov powersave' EXIT
setgov performance
record_window "$RAW/lock-windows/before.txt"
IFS='|' read -r -a ARTS <<< '/root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_client|/root/worktree/zlink-v21-s033/l3868/build-l3868/bench_cpp_zlink_server'
IFS='|' read -r -a SRCS <<< '/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/client|/root/worktree/zlink-v21-s033/l3868/framework/bench/grpc/cpp/zlink'
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
    out="$RAW/${pattern}-${size}/${stage}"
    (( attempt == 1 )) || out+="/attempt${attempt}"
    mkdir -p "$out"
    snapshot
    meta="$BUILD_META"
    printf -- '- %s %s-%s %s built{%s}; receive{binding=bench_zlink_cpp_server.cpp:41,84 recv(none); L0=raw_mesh_node_owner.cpp:540} reply{binding=bench_zlink_cpp_server.cpp:58,67; L0=ladder_wire.hpp:243,252}; costs{per message binding/L0: one blocking recv, source completion poll/observer, new envelope/reply body copy, protobuf request/reply encode, stop.load and metrics record; no added binding allocation/atomic/copy; L1 alone adds queue permit}; raw=%s\n' "$LANG" "$pattern" "$size" "$kind" "$meta" "$out" >> "$PROGRESS"
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
assert 'scheduling_invalid=' not in t
print(c['throughput_per_second'])
CHECK
    rate=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["cells"][0]["throughput_per_second"])' "$result")
    printf -- '- result %s %s-%s %s msg/s=%s raw=%s scheduling=%s\n' "$LANG" "$pattern" "$size" "$kind" "$rate" "$result" "$sched" >> "$PROGRESS"
    break
  done
}
for pair in 'send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096'; do
  read -r pattern size <<<"$pair"
  one_run "$pattern" "$size" warm warm
  one_run "$pattern" "$size" counted counted
  rate=$(python3 - "$RAW/$pattern-$size/counted/run1/$IMPL-$pattern-$size/results.json" <<'RATE'
import json,sys
print(json.load(open(sys.argv[1]))['cells'][0]['throughput_per_second'])
RATE
)
  rawpath="$RAW/$pattern-$size/counted/run1/$IMPL-$pattern-$size/results.json"
  printf '%s\n' "  | $LANG | $pattern-$size | $rate | — | [raw]($rawpath) |" >> "$JOB/report.md"
done
record_window "$RAW/lock-windows/after.txt"
