#!/bin/bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/psmeas
base=/root/worktree/zlink-v21-s033/psmeas-base
branch=/root/worktree/zlink-v21-s033/psmeas-branch
before=${BEFORE:-$(date +%s)}
check_disk_cap() {
  local bytes
  bytes=$( { du -sb "$job" "$base/.psmeas-build" "$branch/.psmeas-build" 2>/dev/null || true; } | awk '{sum += $1} END {print sum}')
  ((bytes < 25 * 1024 * 1024 * 1024)) || { echo "job plus build directories exceed 25 GiB: $bytes bytes" >&2; exit 2; }
}
bash "$job/configure-base.sh" >"$job/configure-base.log" 2>&1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$base/.psmeas-build" --parallel 3 --target bench_cpp_client bench_cpp_framework_server bench_cpp_zlink_server bench_cpp_grpc_server bench_cpp_pm_server >"$job/build-base.log" 2>&1
check_disk_cap
bash "$job/configure-branch.sh" >"$job/configure-branch.log" 2>&1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build "$branch/.psmeas-build" --parallel 3 --target bench_cpp_client bench_cpp_framework_server bench_cpp_zlink_server bench_cpp_grpc_server bench_cpp_pm_server >"$job/build-branch.log" 2>&1
check_disk_cap
for variant in base branch; do
  build="/root/worktree/zlink-v21-s033/psmeas-${variant}/.psmeas-build"
  for binary in bench_cpp_client bench_cpp_framework_server bench_cpp_zlink_server bench_cpp_grpc_server bench_cpp_pm_server; do
    test -x "$build/$binary"
    test "$(stat -c %Y "$build/$binary")" -ge "$before"
  done
done
strings "$base/.psmeas-build/bench_cpp_client" | grep -F -- --target-rate >/dev/null
strings "$branch/.psmeas-build/bench_cpp_client" | grep -F -- --target-rate >/dev/null
bash "$job/measure.sh"
python3 "$job/summarize.py"
test -s "$job/report.md"
rm -rf "$base/.psmeas-build" "$branch/.psmeas-build"
