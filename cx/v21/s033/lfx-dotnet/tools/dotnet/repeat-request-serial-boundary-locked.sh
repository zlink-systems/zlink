#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/lfx-dotnet
root=/root/worktree/zlink-v21-s033/lfx-dotnet
setgov(){ for cpu in $(seq 0 15); do echo "$1" > /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; }
trap 'setgov powersave' EXIT
setgov performance
mkdir -p "$job/raw/lock-windows"
{ date -u +%FT%TZ; uptime; for cpu in $(seq 0 15); do printf 'cpu%s.governor=' "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; } > "$job/raw/lock-windows/L1-L2-serial-boundary-before.txt"
run_one() {
  local level="$1" size="$2" stage="$3" attempt="$4" stamp out result rate commit dirty base artifacts=() artifact
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
  rate=$("$job/tools/dotnet/run-one.sh" request-serial "$size" "$out" "$stamp" 10 "$level")
  result="$out/run1/zlink-dotnet-request-serial-$size/results.json"
  printf -- '- repeat level=%s cell=request-serial-%s stage=%s msg/s=%s raw=%s scheduling=%s\n' "$level" "$size" "$stage" "$rate" "$result" "${result%results.json}scheduling.txt" >> "$job/progress.md"
}
for size in 1024 4096; do
  for level in L1 L2; do
    run_one "$level" "$size" warm 2
    run_one "$level" "$size" counted 2
    run_one "$level" "$size" counted 3
  done
done
{ date -u +%FT%TZ; uptime; for cpu in $(seq 0 15); do printf 'cpu%s.governor=' "$cpu"; cat /sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; done; } > "$job/raw/lock-windows/L1-L2-serial-boundary-after.txt"
