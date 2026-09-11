#!/usr/bin/env bash

set -euo pipefail

TEST_ROOT=/tmp/zlink-session-setup-test.$$
SOURCE_ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
SESSION_SETUP="$SOURCE_ROOT/scripts/dev/session-setup.sh"
PASS=0

cleanup() {
    if [[ -n "${RUNNER_PID:-}" ]]; then
        kill "$RUNNER_PID" 2>/dev/null || true
    fi
    [[ "$TEST_ROOT" == /tmp/zlink-session-setup-test.* ]] && rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

pass() {
    PASS=$((PASS + 1))
    printf 'ok %d - %s\n' "$PASS" "$1"
}

assert_contains() {
    local file=$1 pattern=$2
    grep -Fq -- "$pattern" "$file" || fail "$file 에서 '$pattern'을 찾지 못함"
}

assert_success() {
    local description=$1
    shift
    if ! "$@" >"$TEST_ROOT/out" 2>"$TEST_ROOT/err"; then
        sed -n '1,120p' "$TEST_ROOT/out" >&2
        sed -n '1,120p' "$TEST_ROOT/err" >&2
        fail "$description"
    fi
}

assert_failure() {
    local description=$1 expected=$2 actual
    shift 2
    set +e
    "$@" >"$TEST_ROOT/out" 2>"$TEST_ROOT/err"
    actual=$?
    set -e
    [[ "$actual" -eq "$expected" ]] || fail "$description: exit $actual (expected $expected)"
}

make_fake_commands() {
    mkdir -p "$TEST_ROOT/bin"
    cat >"$TEST_ROOT/bin/sysctl" <<'FAKE_SYSCTL'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"${FAKE_STATE:?}/sysctl.calls"
if [[ "$1" == -n ]]; then cat "${FAKE_STATE}/reserved"; exit 0; fi
[[ "$1" == -w ]] || exit 2
printf '%s\n' "${2#*=}" >"${FAKE_STATE}/reserved"
FAKE_SYSCTL
    cat >"$TEST_ROOT/bin/df" <<'FAKE_DF'
#!/usr/bin/env bash
set -euo pipefail
cat "${FAKE_STATE:?}/df.out"
FAKE_DF
    chmod +x "$TEST_ROOT/bin/sysctl" "$TEST_ROOT/bin/df"
}

run_setup() {
    env HOME="$TEST_ROOT/home" \
        FAKE_STATE="$TEST_ROOT/state" \
        ZLINK_SYSCTL_BIN="$TEST_ROOT/bin/sysctl" \
        ZLINK_DF_BIN="$TEST_ROOT/bin/df" \
        ZLINK_TMP_DIR="$TEST_ROOT/tmp" \
        ZLINK_TMPFS_MIN_FREE_KB=1024 \
        ZLINK_JOB_TMP_MAX_AGE_DAYS=1 \
        ZLINK_PERF_QUEUE="$TEST_ROOT/queue" \
        ZLINK_LOCAL_PACKAGE_ROOT="$TEST_ROOT/packages" \
        ZLINK_CORE_CACHE_DIR="$TEST_ROOT/core-cache" \
        ZLINK_CORE_RELEASE_PLATFORM=linux-x64 \
        "$SESSION_SETUP" "$@"
}

mkdir -p "$TEST_ROOT/state" "$TEST_ROOT/tmp" "$TEST_ROOT/home" "$TEST_ROOT/queue" "$TEST_ROOT/packages"
make_fake_commands
printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\ntmpfs 4096 3072 1024 75%% /tmp\n' >"$TEST_ROOT/state/df.out"
printf '5200-5299,6200-6219\n' >"$TEST_ROOT/state/reserved"

assert_success '정상 상태를 확인한다' run_setup --check
assert_contains "$TEST_ROOT/out" '벤치 포트'
assert_contains "$TEST_ROOT/out" '예약됨'
assert_contains "$TEST_ROOT/out" 'runner 없음 (시작하지 않음)'
pass '--check은 상태 표와 runner 경고를 낸다'

printf '\n' >"$TEST_ROOT/state/reserved"
assert_success '기본 실행이 누락 포트를 예약한다' run_setup
assert_contains "$TEST_ROOT/state/reserved" '5200-5299,6200-6219'
assert_contains "$TEST_ROOT/out" '예약 설정됨'
pass '기본 실행은 포트 예약만 보정한다'

printf '\n' >"$TEST_ROOT/state/reserved"
assert_success '--check은 sysctl을 쓰지 않는다' run_setup --check
[[ -z "$(tr -d '[:space:]' <"$TEST_ROOT/state/reserved")" ]] || fail '--check이 포트 예약을 변경했다'
assert_contains "$TEST_ROOT/out" '미예약 (--check: 변경하지 않음)'
pass '--check은 변경하지 않는다'

assert_success '--dry-run은 sysctl을 쓰지 않는다' run_setup --dry-run
[[ -z "$(tr -d '[:space:]' <"$TEST_ROOT/state/reserved")" ]] || fail '--dry-run이 포트 예약을 변경했다'
assert_contains "$TEST_ROOT/out" '[dry-run]'
pass '--dry-run은 설정 예정만 표시한다'

mkdir -p "$TEST_ROOT/tmp/zlink-job-old" "$TEST_ROOT/tmp/zlink-job-new"
touch -d '3 days ago' "$TEST_ROOT/tmp/zlink-job-old"
printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\ntmpfs 4096 4000 96 98%% /tmp\n' >"$TEST_ROOT/state/df.out"
assert_success 'tmpfs 여유 부족 후보를 표시한다' run_setup --check
assert_contains "$TEST_ROOT/out" '여유 부족'
assert_contains "$TEST_ROOT/out" "$TEST_ROOT/tmp/zlink-job-old"
[[ -d "$TEST_ROOT/tmp/zlink-job-old" ]] || fail '후보 디렉터리를 삭제했다'
pass 'tmpfs 여유 부족 시 오래된 후보만 표시한다'

sleep 30 &
RUNNER_PID=$!
printf '%s\n' "$RUNNER_PID" >"$TEST_ROOT/queue/runner.pid"
assert_success '기록한 pid로 runner 상태를 확인한다' run_setup --check
assert_contains "$TEST_ROOT/out" "실행 중 (pid $RUNNER_PID)"
pass '큐 runner는 기록한 pid만 확인한다'
kill "$RUNNER_PID"
wait "$RUNNER_PID" 2>/dev/null || true
RUNNER_PID=""
rm "$TEST_ROOT/queue/runner.pid"

core_version=$(sed -n 's/^LIBZLINK_VERSION=//p' "$SOURCE_ROOT/VERSION")
mkdir -p "$TEST_ROOT/core-cache/$core_version/linux-x64/share/zlink"
printf '{"version":"%s"}\n' "$core_version" \
    >"$TEST_ROOT/core-cache/$core_version/linux-x64/share/zlink/core-package-provenance.json"
mkdir -p "$TEST_ROOT/packages/nuget"
dotnet_binding_version=$(sed -n 's/^ZLINK_BINDING_VERSION=//p' "$SOURCE_ROOT/bindings/dotnet/VERSION")
touch "$TEST_ROOT/packages/nuget/Zlink.$dotnet_binding_version.nupkg"
printf '5200-5299,6200-6219\n' >"$TEST_ROOT/state/reserved"
printf 'Filesystem 1024-blocks Used Available Capacity Mounted on\ntmpfs 4096 0 4096 0%% /tmp\n' >"$TEST_ROOT/state/df.out"
assert_success 'Core와 로컬 패키지 존재를 확인한다' run_setup --check
assert_contains "$TEST_ROOT/out" 'Core release'
assert_contains "$TEST_ROOT/out" '준비됨'
assert_contains "$TEST_ROOT/out" '존재 (1/8 언어'
pass 'Core prefix와 로컬 패키지를 표시한다'

assert_failure '잘못된 인자는 exit 2' 2 run_setup --unknown
pass '잘못된 인자는 exit 2이다'
