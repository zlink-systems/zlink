#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/lad-dotnet-s1
root=/root/worktree/zlink-v21-s033/lad-dotnet
levels=("$@")
start=$SECONDS
record_window(){ { date -u +%FT%TZ; uptime; for cpu in $(seq 0 15); do printf 'cpu%s.governor=' "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; } > "$1"; }
has_valid_counted() {
  local level="$1" pattern="$2" size="$3" impl
  [[ "$level" == L5_7 ]] && impl=zlink-framework-dotnet || impl=zlink-dotnet
  python3 - "$job/raw/$level/$pattern-$size/counted" "$impl-$pattern-$size" <<'CHECK'
import json,pathlib,sys
base=pathlib.Path(sys.argv[1]); stem=sys.argv[2]
def within(value,low,high):
    try:
        for part in value.split(','):
            nums=[int(x) for x in part.split('-')]
            if len(nums) not in (1,2) or not low<=nums[0]<=nums[-1]<=high: return False
        return True
    except Exception: return False
for raw in base.glob('attempt*/run1/'+stem+'/results.json'):
    sched=raw.with_name('scheduling.txt')
    if not sched.is_file(): continue
    try:
        cell=json.loads(raw.read_text())['cells'][0]; txt=sched.read_text()
        if cell['errors'] or cell['server_errors'] or cell['target_stats']['errors'] or cell['submitted']!=cell['completed'] or cell['completed']!=cell['target_stats']['received']: continue
        if 'scheduling_invalid=' in txt or 'affinity_invalid=' in txt: continue
        ok=True; lines=txt.splitlines()
        for phase,lo,hi in (('source_start',0,5),('mid_active_source',0,5),('target_start',8,13),('mid_active_target',8,13)):
            tids=[x for x in lines if x.startswith(phase+'_tid=')]
            aff=[x.rsplit('cpus=',1)[1] for x in lines if x.startswith(phase+'_affinity ') and 'cpus=' in x]
            if not tids or len(tids)!=len(aff) or not all(within(x,lo,hi) for x in aff): ok=False; break
            if not all('current scheduling policy: SCHED_OTHER' in '\n'.join(lines[i:i+3]) for i,x in enumerate(lines) if x.startswith(phase+'_tid=')): ok=False; break
        if ok: raise SystemExit(0)
    except Exception: pass
raise SystemExit(1)
CHECK
}
setgov(){ for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
trap 'setgov powersave' EXIT
setgov performance
mkdir -p "$job/raw/lock-windows"
record_window "$job/raw/lock-windows/$(IFS=-; echo "${levels[*]}")-before.txt"
for level in "${levels[@]}"; do
  [[ "$level" == binding || "$level" == L0 || "$level" == L1 || "$level" == L2 || "$level" == L3 || "$level" == L4 || "$level" == L5_7 ]] || { echo "invalid level $level" >&2; exit 2; }
  for pair in 'send-saturation 1024' 'send-saturation 4096' 'request-serial 1024' 'request-serial 4096' 'request-backpressure 1024' 'request-backpressure 4096'; do
    read -r pattern size <<<"$pair"
    if has_valid_counted "$level" "$pattern" "$size"; then
      printf -- '- skip existing valid counted result level=%s cell=%s-%s\n' "$level" "$pattern" "$size" >> "$job/progress.md"
      continue
    fi
    for stage in warm counted; do
      (( SECONDS - start < 1500 )) || { echo '25-minute stop boundary reached' >&2; exit 124; }
      stamp="dotnet-${level}-${pattern}-${size}-${stage}-$(date -u +%Y%m%dT%H%M%SZ)"
      stage_root="$job/raw/$level/$pattern-$size/$stage"
      mkdir -p "$stage_root"
      attempt=1
      while [[ -e "$stage_root/attempt$attempt" ]]; do attempt=$((attempt+1)); done
      out="$stage_root/attempt$attempt"
      mkdir -p "$out"
      commit=$(git -C "$root" rev-parse HEAD)
      dirty=clean; [[ -z "$(git -C "$root" status --porcelain)" ]] || dirty=dirty
      base=$(git -C "$root" merge-base HEAD origin/main)
      artifacts=()
      for artifact in framework/bench/grpc/dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll framework/bench/grpc/dotnet/ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll framework/bench/grpc/dotnet/ZLinkRawServer/bin/Release/net8.0/WithGrpcBench.ZLinkRawServer.dll; do
        [[ -f "$root/$artifact" ]] || { echo "missing artifact $artifact" >&2; exit 1; }
        artifacts+=("$artifact@$(stat -c %y "$root/$artifact")#$(sha256sum "$root/$artifact" | cut -d' ' -f1)")
      done
      for artifact in "${artifacts[@]}"; do
        path=${artifact%%@*}
        case "$path" in
          *Client.dll) srcdirs=(framework/bench/grpc/dotnet/Client framework/bench/grpc/dotnet/Shared framework/languages/dotnet/src) ;;
          *ZLinkServer.dll) srcdirs=(framework/bench/grpc/dotnet/ZLinkServer framework/bench/grpc/dotnet/Shared framework/languages/dotnet/src) ;;
          *ZLinkRawServer.dll) srcdirs=(framework/bench/grpc/dotnet/ZLinkRawServer framework/bench/grpc/dotnet/Shared) ;;
        esac
        for srcdir in "${srcdirs[@]}"; do
          while IFS= read -r source; do [[ "$root/$path" -nt "$source" ]] || { echo "stale artifact $path <= $source" >&2; exit 1; }; done < <(find "$root/$srcdir" -type f \( -name '*.cs' -o -name '*.props' -o -name '*.csproj' \))
        done
      done
      printf -- '- %s level=%s cell=%s-%s stage=%s HEAD=%s dirty=%s merge-base=%s artifacts=%s\n' "$(date -u +%FT%TZ)" "$level" "$pattern" "$size" "$stage" "$commit" "$dirty" "$base" "$(IFS=';'; echo "${artifacts[*]}")" >> "$job/progress.md"
      printf -- '  parity binding recv=ZLinkRawServer/Program.cs:64-76 (blocking Recv, same drain/Received/metrics); reply=:103-109 (new protobuf body+raw header, two-part submit); ladder recv=ZLinkServer/Program.cs:208-232 (same blocking Recv, same drain/Received/metrics; L1+ permit before recv); reply=:351-365,462-477 (new body/envelope as selected, two-part submit); per-message waits: 1 blocking Recv each, L1+ one permit acquire/release; allocations: same Received/body/header baseline, L1+ permit, L2+ header parsing/encoding, L3+ service-frame pack/unpack, L4+ JSON encode/decode, L5_7 owner/activation/completion; metrics: same target record; copies: protobuf body path through L3, typed JSON body from L4.\n' >> "$job/progress.md"
      warm=10
      [[ "$stage" == warm ]] && warm=10 || warm=10
      result_rate=$("$job/tools/dotnet/run-one.sh" "$pattern" "$size" "$out" "$stamp" "$warm" "$level")
      result="$out/run1/$( [[ "$level" == L5_7 ]] && echo zlink-framework-dotnet || echo zlink-dotnet )-${pattern}-${size}/results.json"
      sched="${result%results.json}scheduling.txt"
      if [[ "$stage" == counted ]]; then
        printf -- '- result level=%s cell=%s-%s msg/s=%s raw=%s scheduling=%s\n' "$level" "$pattern" "$size" "$result_rate" "$result" "$sched" >> "$job/progress.md"
      else
        printf -- '- warm level=%s cell=%s-%s discarded msg/s=%s raw=%s scheduling=%s\n' "$level" "$pattern" "$size" "$result_rate" "$result" "$sched" >> "$job/progress.md"
      fi
    done
  done
done
record_window "$job/raw/lock-windows/$(IFS=-; echo "${levels[*]}")-after.txt"
