#!/usr/bin/env bash

set -euo pipefail

TEST_ROOT=/tmp/zlink-ci-watch-sh-test.$$
SOURCE_ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
WATCH_SH="$SOURCE_ROOT/scripts/dev/ci-watch.sh"
PASS=0

cleanup() {
    local pid
    if [[ -f "$TEST_ROOT/state/watcher.pid" ]]; then
        IFS= read -r pid <"$TEST_ROOT/state/watcher.pid" || true
        if [[ "$pid" =~ ^[1-9][0-9]*$ ]]; then
            kill -TERM "$pid" >/dev/null 2>&1 || true
        fi
    fi
    [[ "$TEST_ROOT" == /tmp/zlink-ci-watch-sh-test.* ]] && rm -rf -- "$TEST_ROOT"
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

assert_success() {
    local description=$1
    shift
    if ! "$@" >"$TEST_ROOT/last.out" 2>"$TEST_ROOT/last.err"; then
        sed -n '1,120p' "$TEST_ROOT/last.out" >&2
        sed -n '1,120p' "$TEST_ROOT/last.err" >&2
        fail "$description"
    fi
}

assert_failure() {
    local description=$1 expected=$2 actual
    shift 2
    set +e
    "$@" >"$TEST_ROOT/last.out" 2>"$TEST_ROOT/last.err"
    actual=$?
    set -e
    [[ "$actual" -eq "$expected" ]] || fail "$description: exit $actual (expected $expected)"
}

assert_contains() {
    local file=$1 pattern=$2
    grep -Eq -- "$pattern" "$file" || fail "$file 에서 패턴을 찾지 못함: $pattern"
}

wait_for_calls() {
    local wanted=$1 current attempt
    for ((attempt = 0; attempt < 50; attempt++)); do
        current=0
        [[ -f "$TEST_ROOT/gh/calls.log" ]] && current=$(wc -l <"$TEST_ROOT/gh/calls.log")
        ((current >= wanted)) && return 0
        sleep 0.1
    done
    fail "gh 호출 수가 $wanted 에 도달하지 않았습니다."
}

wait_for_stopped() {
    local attempt
    for ((attempt = 0; attempt < 50; attempt++)); do
        if watch status >"$TEST_ROOT/last.out" 2>"$TEST_ROOT/last.err" && \
            grep -q '실행 중인 CI 감시자가 없습니다' "$TEST_ROOT/last.out"; then
            return 0
        fi
        sleep 0.1
    done
    fail '감시자가 종료되지 않았습니다.'
}

mkdir -p "$TEST_ROOT/fake-bin" "$TEST_ROOT/gh" "$TEST_ROOT/sleep" "$TEST_ROOT/state"

cat >"$TEST_ROOT/fake-bin/gh" <<'FAKE_GH'
#!/usr/bin/env bash
set -euo pipefail

state=${FAKE_GH_STATE:?}
printf '%q ' "$@" >>"$state/calls.log"
printf '\n' >>"$state/calls.log"

case "${1:-} ${2:-}" in
    'pr checks')
        cat "$state/pr-output"
        [[ -f "$state/pr-exit" ]] && exit "$(cat "$state/pr-exit")"
        ;;
    'run list')
        cat "$state/branch-output"
        ;;
    *)
        printf 'fake gh: 처리하지 못한 호출: %s\n' "$*" >&2
        exit 2
        ;;
esac
FAKE_GH
chmod +x "$TEST_ROOT/fake-bin/gh"

cat >"$TEST_ROOT/fake-bin/sleep" <<'FAKE_SLEEP'
#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" != 600 ]]; then
    exec /usr/bin/sleep "$@"
fi

state=${FAKE_SLEEP_STATE:?}
printf '600\n' >>"$state/calls.log"
while [[ ! -f "$state/wake" ]]; do
    /usr/bin/sleep 0.05
done
rm -f -- "$state/wake"
FAKE_SLEEP
chmod +x "$TEST_ROOT/fake-bin/sleep"

printf 'build\tpass\n' >"$TEST_ROOT/gh/pr-output"
printf '[{"workflowName":"CI","status":"completed","conclusion":"success"}]\n' >"$TEST_ROOT/gh/branch-output"

watch() {
    env PATH="$TEST_ROOT/fake-bin:$PATH" \
        FAKE_GH_STATE="$TEST_ROOT/gh" \
        FAKE_SLEEP_STATE="$TEST_ROOT/sleep" \
        ZLINK_CI_WATCH_STATE_DIR="$TEST_ROOT/state" \
        "$WATCH_SH" "$@"
}

cd "$SOURCE_ROOT"

assert_failure '대상 없는 start 거부 실패' 2 watch start
assert_contains "$TEST_ROOT/last.err" '재개 명령:'
pass '잘못된 인자는 exit 2와 재개 방법으로 거부한다'

assert_success 'dry-run start 실패' watch --dry-run start --pr 38
[[ ! -f "$TEST_ROOT/gh/calls.log" ]] || fail 'dry-run이 gh를 호출했습니다.'
pass '전역 dry-run은 감시자와 GitHub 호출을 만들지 않는다'

assert_success 'PR 감시자 시작 실패' watch start --pr 38
wait_for_calls 1
assert_contains "$TEST_ROOT/gh/calls.log" '^pr checks 38 '
assert_success '실행 중 status 실패' watch status
assert_contains "$TEST_ROOT/last.out" 'CI 감시자 실행 중: pr'
pass 'PR 감시자는 시작 직후 gh pr checks 한 번으로 상태를 읽는다'

assert_failure '두 번째 감시자 거부 실패' 1 watch start --branch ci/38
[[ $(wc -l <"$TEST_ROOT/gh/calls.log") -eq 1 ]] || fail '두 번째 감시자가 GitHub API를 호출했습니다.'
pass '실행 중인 감시자가 있으면 두 번째 시작을 거부한다'

touch "$TEST_ROOT/sleep/wake"
wait_for_calls 2
[[ $(grep -c 'CI 상태 변경:' "$TEST_ROOT/state/ci-watch.log") -eq 1 ]] || fail '같은 상태를 다시 출력했습니다.'
printf 'build\tfail\n' >"$TEST_ROOT/gh/pr-output"
touch "$TEST_ROOT/sleep/wake"
wait_for_calls 3
[[ $(grep -c 'CI 상태 변경:' "$TEST_ROOT/state/ci-watch.log") -eq 2 ]] || fail '상태 변화가 출력되지 않았습니다.'
pass '각 감시 주기는 gh 한 번만 호출하고 상태 변화만 출력한다'

assert_success '감시자 stop 실패' watch stop
touch "$TEST_ROOT/sleep/wake"
wait_for_stopped
pass 'stop은 기록한 pid의 감시자를 종료한다'

assert_success '브랜치 감시자 시작 실패' watch start --branch ci/38
wait_for_calls 4
assert_contains "$TEST_ROOT/gh/calls.log" '^run list --branch ci/38 --limit 1 --json '
assert_success '브랜치 감시자 stop 실패' watch stop
touch "$TEST_ROOT/sleep/wake"
wait_for_stopped
pass '브랜치 감시자는 gh run list 한 번으로 상태를 읽는다'

printf '모든 ci-watch.sh 테스트 통과 (%d개)\n' "$PASS"
