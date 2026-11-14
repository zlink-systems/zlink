#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/lfx-dotnet
root=/root/worktree/zlink-v21-s033/lfx-dotnet
setgov(){ for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
trap 'setgov powersave' EXIT
setgov performance
mkdir -p "$job/raw/lock-windows"
{ date -u +%FT%TZ; uptime; for cpu in $(seq 0 15); do printf 'cpu%s.governor=' "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; } > "$job/raw/lock-windows/L1-L2-serial-boundary-valid-before.txt"
valid_count() {
 python3 - "$1" "$2" <<'PY'
import json,pathlib,sys
base=pathlib.Path(sys.argv[1]); stem=sys.argv[2]; n=0
for f in base.glob('attempt*/run1/'+stem+'/results.json'):
 s=f.with_name('scheduling.txt')
 if not s.is_file(): continue
 try:
  r=json.loads(f.read_text())['cells'][0]; t=s.read_text()
  if r['errors'] or r['server_errors'] or r['target_stats']['errors'] or r['submitted']!=r['completed'] or r['completed']!=r['target_stats']['received']: continue
  if 'scheduling_invalid=' in t or 'affinity_invalid=' in t: continue
  n+=1
 except Exception: pass
print(n)
PY
}
next_attempt(){
 local stage_root="$1" max=0 path n
 mkdir -p "$stage_root"
 for path in "$stage_root"/attempt*; do [[ -d "$path" ]] || continue; n=${path##*attempt}; [[ "$n" =~ ^[0-9]+$ ]] && ((n>max)) && max=$n; done
 echo $((max+1))
}
run_one() {
  local level="$1" size="$2" stage="$3" attempt="$4" stamp out result rate commit dirty base artifacts=() artifact rc
  stamp="dotnet-${level}-request-serial-${size}-${stage}-${attempt}-$(date -u +%Y%m%dT%H%M%SZ)"
  out="$job/raw/$level/request-serial-$size/$stage/attempt$attempt"
  mkdir -p "$out"
  commit=$(git -C "$root" rev-parse HEAD)
  dirty=clean; [[ -z "$(git -C "$root" status --porcelain)" ]] || dirty=dirty
  base=$(git -C "$root" merge-base HEAD origin/main)
  for artifact in framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll; do
    artifacts+=("$artifact@$(stat -c %y "$root/$artifact")#$(sha256sum "$root/$artifact" | cut -d' ' -f1)")
  done
  printf -- '- %s level=%s cell=request-serial-%s stage=%s HEAD=%s dirty=%s merge-base=%s artifacts=%s\n' "$(date -u +%FT%TZ)" "$level" "$size" "$stage" "$commit" "$dirty" "$base" "$(IFS=';'; echo "${artifacts[*]}")" >> "$job/progress.md"
  printf -- '  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding; metrics: same target record; copies: protobuf body.\n' >> "$job/progress.md"
  set +e
  rate=$("$job/tools/dotnet/run-one.sh" request-serial "$size" "$out" "$stamp" 10 "$level" 2>&1)
  rc=$?
  set -e
  result="$out/run1/zlink-dotnet-request-serial-$size/results.json"
  printf -- '- repeat level=%s cell=request-serial-%s stage=%s attempt=%s rc=%s output=%s raw=%s scheduling=%s\n' "$level" "$size" "$stage" "$attempt" "$rc" "$rate" "$result" "${result%results.json}scheduling.txt" >> "$job/progress.md"
  return "$rc"
}
for size in 1024 4096; do
  for level in L1 L2; do
    tries=0
    while :; do
      attempt=$(next_attempt "$job/raw/$level/request-serial-$size/warm")
      tries=$((tries+1)); ((tries<=5)) || { echo "warm retries exhausted: $level $size" >&2; exit 1; }
      if run_one "$level" "$size" warm "$attempt"; then break; fi
    done
    tries=0
    while (( $(valid_count "$job/raw/$level/request-serial-$size/counted" "zlink-dotnet-request-serial-$size") < 3 )); do
      attempt=$(next_attempt "$job/raw/$level/request-serial-$size/counted")
      tries=$((tries+1)); ((tries<=6)) || { echo "count retries exhausted: $level $size" >&2; exit 1; }
      run_one "$level" "$size" counted "$attempt" || true
    done
  done
done
{ date -u +%FT%TZ; uptime; for cpu in $(seq 0 15); do printf 'cpu%s.governor=' "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; } > "$job/raw/lock-windows/L1-L2-serial-boundary-valid-after.txt"
