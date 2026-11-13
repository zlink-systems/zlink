#!/bin/bash
# PR을 올리거나 다시 push하기 전, 머지 큐 필수 check(.github/workflows/framework-{cpp,java,dotnet,node}.yml)와
# 같은 범위의 시험을 로컬에서 돌린다. 대표님 2026-10-31: "queue 에서 안깨지도록 pr 올릴때 테스트 확실하게 하고"
# usage: pre-pr.sh <worktree>
#   결과 요약: $PRE_PR_LOG_ROOT/<branch>/summary.md (PR 본문에 붙인다). 하나라도 실패하면 exit 1, 준비 조건 실패는 exit 2.
# 환경 변수(기본값은 cloudv):
#   PRE_PR_LOG_ROOT=/root/jobs/fx/pre-pr  PRE_PR_BUILD_CPUS=0-7  PRE_PR_TEST_CPUS=8-15
#   PRE_PR_NODE20=<node 20 bin 폴더>(있으면 Node 20으로 verify:ci 한 번 더)
#   PRE_PR_CPP_TOOLCHAIN=<cmake toolchain 파일>(CI는 system toolchain, 로컬은 vcpkg)
#   PRE_PR_LANGS="cpp" — 특정 언어만 다시 돌린다(요약 파일은 summary-<언어>.md로 따로 남긴다)
#   PRE_PR_CPP_CORE_PREFIX / PRE_PR_CPP_BINDING_PREFIX: Core·C++ binding 설치 경로(CI는 checkout에서 빌드).
#     비우면 이 worktree의 debug 빌드 캐시 값 → /root/.cache/zlink/core/<LIBZLINK_VERSION>/linux-x64,
#     /root/.cache/zlink/zlink-cpp/<ver>, /root/worktree/*/.artifacts/*/install/zlink-cpp/<ver> 순서로 찾는다.
set -uo pipefail
W=$(cd "${1:?worktree}" && pwd) || exit 2
cd "$W" || exit 2
LOG_ROOT=${PRE_PR_LOG_ROOT:-/root/jobs/fx/pre-pr}
BUILD_CPUS=${PRE_PR_BUILD_CPUS:-0-7}
TEST_CPUS=${PRE_PR_TEST_CPUS:-8-15}
CPP_TOOLCHAIN=${PRE_PR_CPP_TOOLCHAIN:-/root/.cache/zlink/vcpkg/scripts/buildsystems/vcpkg.cmake}
NODE20=${PRE_PR_NODE20:-}

git fetch -q origin main || exit 2
git merge-base --is-ancestor origin/main HEAD || { echo "main이 병합되지 않았습니다: git merge origin/main 먼저"; exit 2; }
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "커밋 안 된 변경이 있습니다"; exit 2; }
branch=$(git branch --show-current); head=$(git rev-parse --short=10 HEAD); main=$(git rev-parse --short=10 origin/main)
changed=$(git diff --name-only origin/main...HEAD)

# 언어 판정: 언어 폴더 + 네 언어가 함께 쓰는 공유 경로(하나라도 있으면 네 언어 전부).
langs=$(sed -nE 's#^framework/languages/(cpp|node|java|dotnet)/.*#\1#p' <<< "$changed" | sort -u)
shared_re='^(framework/runtime/|framework/doc/framework/common/(spec|e2e)/|bindings/|core/|scripts/ci/|\.github/workflows/|VERSION$)'
shared=$(grep -E "$shared_re" <<< "$changed" | head -3)
[ -n "$shared" ] && langs="cpp dotnet java node"
langs=${PRE_PR_LANGS:-$langs}  # 일부 언어만 다시 돌릴 때(요약에 그대로 남는다)
# cross-language smoke: wire·envelope·runtime 공유 경로가 바뀌면 실행.
smoke_re='^(framework/runtime/|framework/doc/framework/common/(spec|e2e)/|bindings/|core/)|/(wire|envelope)[^/]*/'
smoke=$(grep -cE "$smoke_re" <<< "$changed")

L=$LOG_ROOT/$branch; mkdir -p "$L"; out="$L/summary${PRE_PR_LANGS:+-${PRE_PR_LANGS// /-}}.md"
script_sha=$(sha256sum "$0" | cut -c1-12)
{
  echo "로컬 전체 시험 (pre-pr.sh $script_sha, HEAD $head, main $main 병합 상태, $(date -u +%m-%dT%H:%MZ))"
  echo "- 대상 언어: ${langs:-없음}$([ -n "$shared" ] && echo " (공유 경로 변경: $(tr '\n' ' ' <<< "$shared"))")"
} > "$out"
fail=0
record () { # <이름> <로그> <exit>
    local tail_line; tail_line=$(grep -aE "tests passed|tests failed|Passed!|Failed!|BUILD (SUCCESSFUL|FAILED)|passing|failing|# (pass|fail)" "$2" | tail -1 | cut -c1-160)
    if [ "$3" -eq 0 ]; then echo "- $1: 통과 — $tail_line" >> "$out"; else echo "- $1: **실패** (exit $3, $2) — $tail_line" >> "$out"; fail=1; fi
}
wait_cpp_slot () {
    local start=$SECONDS
    until [ "$(pgrep -c cc1plus)" -eq 0 ] && [ "$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)" -ge 12 ]; do
        (( (SECONDS - start) % 300 < 30 )) && echo "$(date -u +%H:%M) C++ 빌드 자리 대기 $(( (SECONDS - start) / 60 ))분 (cc1plus $(pgrep -c cc1plus)개)" >> "$L/wait.log"
        sleep 30
    done
    [ $SECONDS -gt $((start + 30)) ] && echo "- C++ 빌드 자리 대기: $(( (SECONDS - start) / 60 ))분" >> "$out"
}

for lang in $langs; do
    case $lang in
    cpp)  # framework-cpp.yml: linux-ninja-release, samples·cross-language 끔, ctest -L '^(unit|contract)$'
        d=framework/languages/cpp
        core_ver=$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)
        dbg=$d/build/linux-ninja-vcpkg-debug/CMakeCache.txt
        cache_val () { [ -f $dbg ] && sed -n "s/^$1:PATH=//p" $dbg; }
        core_prefix=${PRE_PR_CPP_CORE_PREFIX:-$(cache_val ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX)}
        core_prefix=${core_prefix:-/root/.cache/zlink/core/$core_ver/linux-x64}
        binding_prefix=${PRE_PR_CPP_BINDING_PREFIX:-$(cache_val ZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX)}
        [ -d "$binding_prefix" ] || binding_prefix=/root/.cache/zlink/zlink-cpp/$core_ver
        [ -d "$binding_prefix" ] || binding_prefix=$(ls -d /root/worktree/*/.artifacts/*/install/zlink-cpp/$core_ver 2>/dev/null | head -1)
        echo "- C++ Core $core_prefix, binding $binding_prefix" >> "$out"
        wait_cpp_slot
        (cd $d && cmake --preset linux-ninja-release -DCMAKE_TOOLCHAIN_FILE="$CPP_TOOLCHAIN" \
            -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX="$core_prefix" -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX="$binding_prefix" \
            -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF \
            && taskset -c "$BUILD_CPUS" cmake --build --preset linux-ninja-release --parallel 4) > "$L/cpp-build.log" 2>&1
        rc=$?; record "C++ Release 빌드" "$L/cpp-build.log" $rc
        if [ $rc -eq 0 ]; then
            (cd $d && taskset -c "$TEST_CPUS" ctest --preset linux-ninja-release -L '^(unit|contract)$' -j 4) > "$L/cpp-ctest.log" 2>&1
            record "C++ ctest --preset linux-ninja-release -L '^(unit|contract)\$'" "$L/cpp-ctest.log" $?
        fi ;;
    java)  # framework-java.yml: private 접근 검사 → core test+integrationTest → samples classes
        python3 scripts/ci/check-java-test-private-access.py --base-ref origin/main > "$L/java-private.log" 2>&1
        record "Java 시험 private 접근 검사" "$L/java-private.log" $?
        (cd framework/languages/java && taskset -c "$TEST_CPUS" ./gradlew --no-daemon --max-workers=2 \
              :zlink-framework-core:test :zlink-framework-core:integrationTest -Pzlink.includeSamples=false \
            && ./gradlew --no-daemon --max-workers=2 -p samples classes) > "$L/java.log" 2>&1
        record "Java core test+integrationTest, samples classes" "$L/java.log" $? ;;
    dotnet)  # framework-dotnet.yml: Debug·Release build, Debug net8.0 + Release net10.0 × Unit·Contract·Stream
        dl=framework/languages/dotnet
        export ZLinkFrameworkTargetFrameworks="net8.0;net10.0"
        (dotnet restore $dl/Zlink.Framework.ci.slnf \
            && dotnet build $dl/Zlink.Framework.ci.slnf -c Debug --no-restore \
            && dotnet build $dl/Zlink.Framework.ci.slnf -c Release --no-restore) > "$L/dotnet-build.log" 2>&1
        rc=$?; record ".NET Debug·Release 빌드" "$L/dotnet-build.log" $rc
        if [ $rc -eq 0 ]; then
            for cfg in "Debug net8.0" "Release net10.0"; do
                (for p in tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj \
                          tests/Zlink.Framework.ContractTests/Zlink.Framework.ContractTests.csproj \
                          tests/Systems.Zlink.Stream.Connector.Tests/Systems.Zlink.Stream.Connector.Tests.csproj; do
                     taskset -c "$TEST_CPUS" bash scripts/ci/dotnet-test.sh $dl/$p $cfg || exit 1; done) > "$L/dotnet-${cfg// /-}.log" 2>&1
                record ".NET Unit·Contract·Stream ($cfg)" "$L/dotnet-${cfg// /-}.log" $?
            done
        fi ;;
    node)  # framework-node.yml: Node 22(+20) verify:ci, WebGL bundle 불변, Chromium E2E
        nd=framework/languages/node
        (cd $nd && npm ci && taskset -c "$TEST_CPUS" npm run verify:ci) > "$L/node22.log" 2>&1
        record "Node $(node -v) npm run verify:ci" "$L/node22.log" $?
        git diff --exit-code -- framework/languages/unity/com.zlink.stream-connector.webgl > "$L/node-webgl.log" 2>&1
        record "WebGL bundle 불변(git diff --exit-code)" "$L/node-webgl.log" $?
        (cd $nd && npm run browser:install:ci && taskset -c "$TEST_CPUS" npm run test:browser) > "$L/node-browser.log" 2>&1
        record "Node Chromium E2E (test:browser)" "$L/node-browser.log" $?
        if [ -n "$NODE20" ]; then
            (cd $nd && PATH="$NODE20:$PATH" npm ci && PATH="$NODE20:$PATH" taskset -c "$TEST_CPUS" npm run verify:ci) > "$L/node20.log" 2>&1
            record "Node 20 npm run verify:ci" "$L/node20.log" $?
            (cd $nd && npm ci) > /dev/null 2>&1
        else
            echo "- Node 20: 이 머신에 없음(PRE_PR_NODE20 미설정) — 큐에서만 확인" >> "$out"
        fi ;;
    esac
done

if [ "$smoke" -gt 0 ]; then  # framework-node.yml cross-language smoke
    (cd framework/languages/node && npm run build && npm run verify:cross-language) > "$L/smoke.log" 2>&1
    record "cross-language smoke (verify:cross-language)" "$L/smoke.log" $?
fi
bash scripts/format/format.sh --check $langs > "$L/format.log" 2>&1
record "format.sh --check ${langs:-(전체)}" "$L/format.log" $?
cat "$out"
exit $fail
