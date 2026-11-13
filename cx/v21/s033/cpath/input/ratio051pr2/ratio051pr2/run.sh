#!/usr/bin/env bash
# C++ serial request PR (3867 encode once) before/after. before = main 213e579f52 in zlink-ratio040, after = zlink-3867e fee351d0e4.
# Condition B, 6 C++ cells, Framework + binding, 5 runs, median and spread. Pin patch applied to after for the run, reverted at the end.
set -uo pipefail
J=/root/jobs/fx/cx/ratio051pr2
git -C /root/worktree/zlink-ratio040 checkout -q . && git -C /root/worktree/zlink-ratio040 checkout -q --detach 213e579f52 && git -C /root/worktree/zlink-ratio040 apply /root/jobs/fx/cx/ratio040/pin.patch || { echo "before checkout failed"; exit 1; }
git -C /root/worktree/zlink-3867e apply /root/jobs/fx/cx/ratio040/pin.patch || { echo "after pin failed"; exit 1; }
trap 'git -C /root/worktree/zlink-3867e apply -R /root/jobs/fx/cx/ratio040/pin.patch && echo "after pin reverted"' EXIT
for side in before after; do
  if [ $side = before ]; then R=/root/worktree/zlink-ratio040; else R=/root/worktree/zlink-3867e; fi
  G=$R/framework/bench/grpc
  echo "== $side packages $(date +%T)"
  (cd $R && flock -o /root/jobs/fx/cx/cpp-build.lock /root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 scripts/local-package/build-wsl.sh cpp) > $J/pkg-$side.log 2>&1 || { echo "pkg $side failed"; tail -5 $J/pkg-$side.log; exit 1; }
  echo "== $side bench build $(date +%T)"
  (cd $G && flock -o /root/jobs/fx/cx/cpp-build.lock /root/jobs/fx/cx/measure-shared.sh taskset -c 0-7 bash cpp/build.sh) > $J/build-$side.log 2>&1 || { echo "build $side failed"; tail -5 $J/build-$side.log; exit 1; }
  /root/jobs/fx/cx/bench-preflight.sh $R cpp > $J/preflight-$side.log 2>&1 || { cat $J/preflight-$side.log; echo "preflight $side FAILED"; exit 1; }
done
uptime > $J/load-start.txt
for side in before after; do
  if [ $side = before ]; then R=/root/worktree/zlink-ratio040; else R=/root/worktree/zlink-3867e; fi
  G=$R/framework/bench/grpc
  echo "== measure $side $(date +%T)"
  (cd $G && flock -o /root/jobs/fx/cx/measure.lock env IMPLEMENTATIONS="zlink-cpp,zlink-framework-cpp" RUNS=5 WARMUP_SECONDS=30 DURATION_SECONDS=5 SKIP_BUILD=1 RUN_STAMP=ratio051$side OUTPUT_DIR=$J/log/$side/cpp bash cpp/run_local.sh) > $J/measure-$side.log 2>&1 || echo "FAILED $side"
done
uptime > $J/load-end.txt
python3 /root/jobs/fx/cx/ratio040/table.py $J/log/after $J/log/before | grep -E "^\| (cpp \||언어|---)|^cells" > $J/table.md; cat $J/table.md
echo "== done $(date +%T)"
