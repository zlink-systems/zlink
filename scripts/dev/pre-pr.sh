#!/usr/bin/env bash
set -uo pipefail

usage() {
    cat <<'EOF'
사용법:
  pre-pr.sh <worktree>

환경 변수:
  PRE_PR_LOG_ROOT       로그 루트 (기본: ${TMPDIR:-/tmp}/zlink-pre-pr)
  PRE_PR_BUILD_CPUS     빌드 CPU 고정 범위 (설정하고 taskset이 있을 때만 적용)
  PRE_PR_TEST_CPUS      시험 CPU 고정 범위 (설정하고 taskset이 있을 때만 적용)
  PRE_PR_NODE20         Node 20 실행 파일 디렉터리 (설정하면 verify:ci 추가 실행)
  PRE_PR_CPP_TOOLCHAIN  vcpkg CMake toolchain 파일 (기본: VCPKG_ROOT에서 찾음)
  PRE_PR_LANGS          실행할 언어 목록 (예: "cpp", 기본: 변경 파일로 판정)

결과 요약은 <로그 루트>/<branch>/summary.md에 기록한다.
시험 실패 시 exit 1, 준비 조건 실패 시 exit 2를 반환한다.
EOF
}

if [[ $# -ne 1 ]]; then
    usage >&2
    exit 2
fi
W=$(cd -- "$1" && pwd) || {
    printf '오류: worktree 경로를 확인할 수 없습니다: %s\n' "$1" >&2
    exit 2
}
cd "$W" || exit 2
LOG_ROOT=${PRE_PR_LOG_ROOT:-${TMPDIR:-/tmp}/zlink-pre-pr}
BUILD_CPUS=${PRE_PR_BUILD_CPUS:-}
TEST_CPUS=${PRE_PR_TEST_CPUS:-}
NODE20=${PRE_PR_NODE20:-}
CPP_TOOLCHAIN=
CPP_CORE_PREFIX=
CPP_BINDING_PREFIX=
CPP_LOCAL_PACKAGE_ROOT=

run_with_optional_affinity() {
    local cpus=$1
    shift
    if [[ -n "$cpus" ]] && command -v taskset >/dev/null 2>&1; then
        taskset -c "$cpus" "$@"
    else
        "$@"
    fi
}

resolve_cpp_inputs() {
    local vcpkg_root core_version binding_version artifact_root
    vcpkg_root=${VCPKG_ROOT:-${VCPKG_INSTALLATION_ROOT:-}}
    if [[ -z "$vcpkg_root" && -n "${PRE_PR_CPP_TOOLCHAIN:-}" && "${PRE_PR_CPP_TOOLCHAIN}" == */scripts/buildsystems/vcpkg.cmake ]]; then
        vcpkg_root=${PRE_PR_CPP_TOOLCHAIN%/scripts/buildsystems/vcpkg.cmake}
    fi
    if [[ -z "$vcpkg_root" ]]; then
        printf '오류: C++ 시험에는 VCPKG_ROOT 또는 VCPKG_INSTALLATION_ROOT가 필요합니다.\n' >&2
        printf '       vcpkg를 준비하고 VCPKG_ROOT를 설정하거나 PRE_PR_CPP_TOOLCHAIN을 지정하세요.\n' >&2
        return 2
    fi
    if [[ "$vcpkg_root" != /* ]]; then
        vcpkg_root="$W/$vcpkg_root"
    fi
    export VCPKG_ROOT="$vcpkg_root"
    CPP_TOOLCHAIN=${PRE_PR_CPP_TOOLCHAIN:-$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake}
    if [[ "$CPP_TOOLCHAIN" != /* ]]; then
        CPP_TOOLCHAIN="$W/$CPP_TOOLCHAIN"
    fi
    if [[ ! -f "$CPP_TOOLCHAIN" ]]; then
        printf '오류: vcpkg CMake toolchain 파일을 찾을 수 없습니다: %s\n' "$CPP_TOOLCHAIN" >&2
        return 2
    fi

    artifact_root=${ZLINK_LOCAL_PACKAGE_ROOT:-$W/.artifacts/wsl}
    if [[ "$artifact_root" != /* ]]; then
        artifact_root="$W/$artifact_root"
    fi
    core_version=$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)
    binding_version=$(sed -n 's/^ZLINK_BINDING_VERSION=//p' bindings/cpp/VERSION)
    if [[ -z "$core_version" || -z "$binding_version" ]]; then
        printf '오류: VERSION 또는 bindings/cpp/VERSION에서 package 버전을 읽지 못했습니다.\n' >&2
        return 2
    fi
    CPP_LOCAL_PACKAGE_ROOT=$artifact_root
    CPP_CORE_PREFIX="$artifact_root/install/zlink-core/$core_version"
    CPP_BINDING_PREFIX="$artifact_root/install/zlink-cpp/$binding_version"
    if [[ ! -d "$CPP_CORE_PREFIX" ]]; then
        printf '오류: Core local package가 없습니다: %s\n' "$CPP_CORE_PREFIX" >&2
        printf '       scripts/local-package/build-wsl.sh cpp 로 package를 준비하세요.\n' >&2
        return 2
    fi
    if [[ ! -d "$CPP_BINDING_PREFIX" ]]; then
        printf '오류: C++ binding local package가 없습니다: %s\n' "$CPP_BINDING_PREFIX" >&2
        printf '       ZLINK_LOCAL_PACKAGE_ROOT를 확인하고 scripts/local-package/build-wsl.sh cpp를 실행하세요.\n' >&2
        return 2
    fi
}

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
    cpp)  # framework-cpp.yml: release build, ctest unit·contract labels
        d=framework/languages/cpp
        if ! resolve_cpp_inputs; then
            exit 2
        fi
        core_ver=$(sed -n 's/^LIBZLINK_VERSION=//p' VERSION)
        binding_ver=$(sed -n 's/^ZLINK_BINDING_VERSION=//p' bindings/cpp/VERSION)
        echo "- C++ Core $CPP_CORE_PREFIX, binding $CPP_BINDING_PREFIX" >> "$out"
        wait_cpp_slot
        (cd "$d" && cmake --preset linux-ninja-release -DCMAKE_TOOLCHAIN_FILE="$CPP_TOOLCHAIN" \
            -DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT="$CPP_LOCAL_PACKAGE_ROOT" \
            -DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION="$core_ver" \
            -DZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION="$binding_ver" \
            -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CORE_PREFIX="$CPP_CORE_PREFIX" \
            -DZLINK_FRAMEWORK_CPP_LOCAL_ZLINK_CPP_PREFIX="$CPP_BINDING_PREFIX" \
            -DZLINK_FRAMEWORK_CPP_BUILD_SAMPLES=OFF -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF \
            && run_with_optional_affinity "$BUILD_CPUS" cmake --build --preset linux-ninja-release --parallel 4) > "$L/cpp-build.log" 2>&1
        rc=$?; record "C++ Release 빌드" "$L/cpp-build.log" "$rc"
        if [ "$rc" -eq 0 ]; then
            (cd "$d" && run_with_optional_affinity "$TEST_CPUS" ctest --preset linux-ninja-release -L '^(unit|contract)$' -j 4) > "$L/cpp-ctest.log" 2>&1
            record "C++ ctest --preset linux-ninja-release -L '^(unit|contract)$'" "$L/cpp-ctest.log" "$?"
        fi ;;
    java)  # framework-java.yml: private 접근 검사 → core test+integrationTest → samples classes
        python3 scripts/ci/check-java-test-private-access.py --base-ref origin/main > "$L/java-private.log" 2>&1
        record "Java 시험 private 접근 검사" "$L/java-private.log" $?
        (cd framework/languages/java && run_with_optional_affinity "$TEST_CPUS" ./gradlew --no-daemon --max-workers=2 \
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
                     run_with_optional_affinity "$TEST_CPUS" bash scripts/ci/dotnet-test.sh "$dl/$p" "$cfg" || exit 1; done) > "$L/dotnet-${cfg// /-}.log" 2>&1
                record ".NET Unit·Contract·Stream ($cfg)" "$L/dotnet-${cfg// /-}.log" "$?"
            done
        fi ;;
    node)  # framework-node.yml: Node 22(+20) verify:ci, WebGL bundle 불변, Chromium E2E
        nd=framework/languages/node
        (cd "$nd" && npm ci && run_with_optional_affinity "$TEST_CPUS" npm run verify:ci) > "$L/node22.log" 2>&1
        rc=$?  # $(node -v) in the label would overwrite $? before it is expanded
        record "Node $(node -v) npm run verify:ci" "$L/node22.log" "$rc"
        git diff --exit-code -- framework/languages/unity/com.zlink.stream-connector.webgl > "$L/node-webgl.log" 2>&1
        record "WebGL bundle 불변(git diff --exit-code)" "$L/node-webgl.log" $?
        (cd "$nd" && npm run browser:install:ci && run_with_optional_affinity "$TEST_CPUS" npm run test:browser) > "$L/node-browser.log" 2>&1
        record "Node Chromium E2E (test:browser)" "$L/node-browser.log" $?
        if [ -n "$NODE20" ]; then
            (cd "$nd" && PATH="$NODE20:$PATH" npm ci && PATH="$NODE20:$PATH" run_with_optional_affinity "$TEST_CPUS" npm run verify:ci) > "$L/node20.log" 2>&1
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
