#!/usr/bin/env bash
# Core 1.24.0 tag condition: verify-package.sh on PR #4166 head (PM 2026-11-10 15:15Z, after scheduler measurement).
set -euo pipefail
J=/root/jobs/fx/cx/v21/s033/vp124
HEAD_SHA=8bc547de3c
while pgrep -f '[p]smeas/measure.sh' >/dev/null || pgrep -f '[p]smeas/summarize.py' >/dev/null; do sleep 60; done
echo "measurement idle $(date -u +%T)"
S=$J/src
git -C /root/zlink fetch -q origin pull/4166/head
[[ -d $S ]] || git -C /root/zlink worktree add --detach "$S" "$HEAD_SHA"
[[ "$(git -C $S rev-parse --short=10 HEAD)" == "$HEAD_SHA" ]]
/root/jobs/fx/cx/cpp-build-slot.sh cmake -S $S/core -B $S/core/build-dev -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTS=OFF -DZLINK_BUILD_TESTS=OFF -DBUILD_BENCHMARKS=OFF -DBUILD_SHARED=ON -DBUILD_STATIC=OFF -DWITH_DOC=OFF > $J/configure.log 2>&1
/root/jobs/fx/cx/cpp-build-slot.sh cmake --build $S/core/build-dev --parallel 4 > $J/build.log 2>&1
ZLINK_CORE_CACHE_DIR=$J/cache bash $S/scripts/gate/materialize-local-core-prefix.sh $S/core/build-dev > $J/materialize.log 2>&1
prefix=$J/cache/1.24.0/linux-x64
bash $S/scripts/local-package/core/verify-package.sh --prefix "$prefix" > $J/verify-package.log 2>&1 && rc=0 || rc=$?
echo "verify-package rc=$rc"
(cd $S && bash scripts/local-package/build-wsl.sh --verify-versions) > $J/verify-versions.log 2>&1 && vv=0 || vv=$?; echo "verify-versions rc=$vv"
sha256sum $prefix/lib/libzlink.so.1.24.0 > $J/runtime.sha256
exit $rc
