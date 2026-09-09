#!/usr/bin/env bash

set -euo pipefail

TEST_ROOT=/tmp/zlink-sol-job-sh/job-sh-test.$$
SOURCE_ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
JOB_SH="$SOURCE_ROOT/scripts/dev/job.sh"
PASS=0
export TEST_ROOT JOB_SH

cleanup() {
    local pid_file pid
    if [[ -d "$TEST_ROOT/repo/.artifacts/codex" ]]; then
        for pid_file in "$TEST_ROOT/repo/.artifacts/codex"/*/.pid; do
            [[ -f "$pid_file" ]] || continue
            IFS= read -r pid <"$pid_file" || true
            if [[ "$pid" =~ ^[1-9][0-9]*$ && "$pid" != "$$" ]]; then
                kill -TERM "$pid" 2>/dev/null || true
            fi
        done
    fi
    [[ "$TEST_ROOT" == /tmp/zlink-sol-job-sh/job-sh-test.* ]] && rm -rf -- "$TEST_ROOT"
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

assert_file_contains() {
    local file=$1 pattern=$2
    grep -Eq -- "$pattern" "$file" || fail "$file 에서 패턴을 찾지 못함: $pattern"
}

assert_success() {
    local description=$1
    shift
    if ! "$@" >"$TEST_ROOT/last.out" 2>"$TEST_ROOT/last.err"; then
        sed -n '1,160p' "$TEST_ROOT/last.out" >&2
        sed -n '1,160p' "$TEST_ROOT/last.err" >&2
        fail "$description"
    fi
}

assert_failure() {
    local description=$1 expected=$2
    shift 2
    local actual
    set +e
    "$@" >"$TEST_ROOT/last.out" 2>"$TEST_ROOT/last.err"
    actual=$?
    set -e
    [[ "$actual" -eq "$expected" ]] || {
        sed -n '1,160p' "$TEST_ROOT/last.out" >&2
        sed -n '1,160p' "$TEST_ROOT/last.err" >&2
        fail "$description: exit $actual (expected $expected)"
    }
}

make_fake_codex() {
    mkdir -p "$TEST_ROOT/fake-bin" "$TEST_ROOT/codex-state"
    cat >"$TEST_ROOT/fake-bin/codex" <<'FAKE_CODEX'
#!/usr/bin/env bash
set -euo pipefail

state=${FAKE_CODEX_STATE:?}
mode=${FAKE_CODEX_MODE:-running}
printf '%s\n' "$*" >>"$state/calls.log"
cat >/dev/null

case "$mode" in
    running)
        trap 'exit 0' TERM INT
        while true; do sleep 1; done
        ;;
    fail)
        printf 'invalid_request_error: requested model is unavailable\n' >&2
        exit 1
        ;;
    complete)
        sleep 2
        printf '완료\n' >"${ZLINK_JOB_DIR:?}/summary.md"
        ;;
    quiet)
        exit 0
        ;;
    *)
        printf 'unknown fake mode: %s\n' "$mode" >&2
        exit 2
        ;;
esac
FAKE_CODEX
    chmod +x "$TEST_ROOT/fake-bin/codex"
}

make_repository() {
    mkdir -p "$TEST_ROOT/home/.codex" "$TEST_ROOT/repo"
    git init --initial-branch=main "$TEST_ROOT/repo" >/dev/null
    git -C "$TEST_ROOT/repo" config user.name tester
    git -C "$TEST_ROOT/repo" config user.email tester@example.test
    printf 'fixture\n' >"$TEST_ROOT/repo/README"
    git -C "$TEST_ROOT/repo" add README
    git -C "$TEST_ROOT/repo" commit -m 'test: seed fixture' >/dev/null
    cat >"$TEST_ROOT/home/.codex/config.toml" <<'CONFIG'
model = "gpt-test"

[tui.model_availability_nux]
gpt-test = 1
"gpt-other" = 1
CONFIG
    printf '테스트 brief\n' >"$TEST_ROOT/brief.md"
}

job() {
    env HOME="$TEST_ROOT/home" \
        PATH="$TEST_ROOT/fake-bin:$PATH" \
        FAKE_CODEX_STATE="$TEST_ROOT/codex-state" \
        FAKE_CODEX_MODE="${FAKE_CODEX_MODE:-running}" \
        ZLINK_JOB_STARTUP_SECONDS=1 \
        "$JOB_SH" "$@"
}

mkdir -p "$TEST_ROOT"
make_fake_codex
make_repository
cd "$TEST_ROOT/repo"

assert_success '정상 start 실패' job start normal --worktree "$TEST_ROOT/repo" --brief "$TEST_ROOT/brief.md"
NORMAL_PID=$(<"$TEST_ROOT/repo/.artifacts/codex/normal/.pid")
kill -0 "$NORMAL_PID" 2>/dev/null || fail '정상 job 프로세스가 실행 중이 아님'
[[ -s "$TEST_ROOT/repo/.artifacts/codex/normal/.meta" ]] || fail '.meta가 생성되지 않음'
assert_file_contains "$TEST_ROOT/repo/.artifacts/codex/normal/.meta" '^model=gpt-test$'
assert_file_contains "$TEST_ROOT/codex-state/calls.log" '-o .*/normal/summary.md -$'
assert_success '실행 상태 조회 실패' job status normal
assert_file_contains "$TEST_ROOT/last.out" $'^normal\tgpt-test\t.*\t실행\t'
assert_failure '실행 중인 이름 재사용 거부 실패' 2 job start normal --worktree "$TEST_ROOT/repo" \
    --brief "$TEST_ROOT/brief.md"
pass '정상 start가 pid·meta를 만들고 status가 실행으로 표시한다'

FAKE_CODEX_MODE=fail assert_failure '즉시 실패 감지 실패' 1 job start instant-fail \
    --worktree "$TEST_ROOT/repo" --brief "$TEST_ROOT/brief.md"
assert_file_contains "$TEST_ROOT/last.err" 'invalid_request_error'
[[ ! -e "$TEST_ROOT/repo/.artifacts/codex/instant-fail/.pid" ]] || fail '즉시 실패 job에 .pid가 남음'
assert_success '실패 로그 조회 실패' job logs instant-fail -n 2
assert_file_contains "$TEST_ROOT/last.out" '^감지된 오류: invalid_request_error'
pass '즉시 실패는 로그 꼬리를 출력하고 pid를 남기지 않는다'

CALLS_BEFORE=$(wc -l <"$TEST_ROOT/codex-state/calls.log")
assert_failure '잘못된 모델 거부 실패' 2 job start invalid-model --worktree "$TEST_ROOT/repo" \
    --brief "$TEST_ROOT/brief.md" --model no-such-model
CALLS_AFTER=$(wc -l <"$TEST_ROOT/codex-state/calls.log")
[[ "$CALLS_BEFORE" -eq "$CALLS_AFTER" ]] || fail '잘못된 모델인데 codex가 실행됨'
assert_file_contains "$TEST_ROOT/last.err" '사용 가능한 모델:'
pass '잘못된 모델 id는 codex 실행 전에 exit 2로 거부한다'

assert_failure '동시 상한 거부 실패' 2 job start over-limit --worktree "$TEST_ROOT/repo" \
    --brief "$TEST_ROOT/brief.md" --max-jobs 1
assert_file_contains "$TEST_ROOT/last.err" 'normal'
pass '동시 실행 상한을 넘으면 현재 실행 목록과 함께 거부한다'

assert_success '두 번째 job 시작 실패' job start other --worktree "$TEST_ROOT/repo" --brief "$TEST_ROOT/brief.md"
OTHER_PID=$(<"$TEST_ROOT/repo/.artifacts/codex/other/.pid")
assert_success '대상 job kill 실패' job kill normal
kill -0 "$NORMAL_PID" 2>/dev/null && fail '대상 job이 종료되지 않음'
kill -0 "$OTHER_PID" 2>/dev/null || fail '다른 job까지 종료됨'
pass 'kill은 기록된 대상 pid만 종료한다'

assert_success '재시작용 최초 start 실패' job start restart --worktree "$TEST_ROOT/repo" --brief "$TEST_ROOT/brief.md"
assert_success '재시작용 kill 실패' job kill restart
printf '이전 로그 표식\n' >>"$TEST_ROOT/repo/.artifacts/codex/restart/job.log"
assert_success '같은 이름 재시작 실패' job start restart --worktree "$TEST_ROOT/repo" --brief "$TEST_ROOT/brief.md"
ARCHIVED_LOG=$(find "$TEST_ROOT/repo/.artifacts/codex/restart" -maxdepth 1 -type f -name 'job.log.*' -print -quit)
[[ -n "$ARCHIVED_LOG" ]] || fail '기존 job.log가 보존되지 않음'
assert_file_contains "$ARCHIVED_LOG" '이전 로그 표식'
assert_success '재시작 job 정리 실패' job kill restart
pass '죽은 job 이름을 재사용하면 기존 로그를 타임스탬프로 보존한다'

CALLS_BEFORE=$(wc -l <"$TEST_ROOT/codex-state/calls.log")
assert_success 'dry-run start 실패' job --dry-run start dry-only --worktree "$TEST_ROOT/repo" \
    --brief "$TEST_ROOT/brief.md"
CALLS_AFTER=$(wc -l <"$TEST_ROOT/codex-state/calls.log")
[[ "$CALLS_BEFORE" -eq "$CALLS_AFTER" ]] || fail 'dry-run이 codex를 실행함'
[[ ! -e "$TEST_ROOT/repo/.artifacts/codex/dry-only" ]] || fail 'dry-run이 job 디렉터리를 만듦'
pass '전역 --dry-run은 프로세스와 파일을 만들지 않는다'

FAKE_CODEX_MODE=complete assert_success 'watch fixture start 실패' job start watch-job \
    --worktree "$TEST_ROOT/repo" --brief "$TEST_ROOT/brief.md"
set +e
FAKE_CODEX_MODE=running timeout 5 bash -c "$(declare -f job); job watch --interval 1" \
    >"$TEST_ROOT/watch.out" 2>"$TEST_ROOT/watch.err"
WATCH_EXIT=$?
set -e
[[ "$WATCH_EXIT" -eq 124 ]] || fail "watch timeout 결과가 예상과 다름: $WATCH_EXIT"
[[ $(grep -c $'watch-job\t.*완료' "$TEST_ROOT/watch.out") -eq 1 ]] \
    || fail 'watch가 완료 전환을 정확히 한 번 출력하지 않음'
[[ $(grep -c $'watch-job\t실행$' "$TEST_ROOT/watch.out") -le 1 ]] \
    || fail 'watch가 실행 상태를 반복 출력함'
pass 'watch는 완료 전환을 한 번만 출력하고 같은 상태를 반복하지 않는다'

assert_failure '없는 brief 거부 실패' 2 job start no-brief --worktree "$TEST_ROOT/repo" \
    --brief "$TEST_ROOT/missing.md"
mkdir -p "$TEST_ROOT/not-worktree"
assert_failure 'git worktree 아닌 경로 거부 실패' 2 job start bad-worktree --worktree "$TEST_ROOT/not-worktree" \
    --brief "$TEST_ROOT/brief.md"
pass 'brief 없음과 git worktree가 아닌 경로를 exit 2로 거부한다'

mkdir -p "$TEST_ROOT/repo/.artifacts/codex/state-complete" \
    "$TEST_ROOT/repo/.artifacts/codex/state-dead" \
    "$TEST_ROOT/repo/.artifacts/codex/state-failed"
printf '완료\n' >"$TEST_ROOT/repo/.artifacts/codex/state-complete/summary.md"
printf '정상 로그\n' >"$TEST_ROOT/repo/.artifacts/codex/state-complete/job.log"
printf '조용히 종료\n' >"$TEST_ROOT/repo/.artifacts/codex/state-dead/job.log"
printf '403 forbidden\n' >"$TEST_ROOT/repo/.artifacts/codex/state-failed/job.log"
assert_success '완료·죽음·실패 상태 조회 실패' job status
assert_file_contains "$TEST_ROOT/last.out" $'^state-complete\t.*\t완료\t있음\t-$'
assert_file_contains "$TEST_ROOT/last.out" $'^state-dead\t.*\t죽음\t없음\t-$'
assert_file_contains "$TEST_ROOT/last.out" $'^state-failed\t.*\t실패\t없음\t403 forbidden$'
pass 'status가 완료·죽음·실패를 계약대로 판정한다'

mkdir -p "$TEST_ROOT/repo/.artifacts/codex/state-reused"
printf '%s\n' "$$" >"$TEST_ROOT/repo/.artifacts/codex/state-reused/.pid"
printf 'proc_start_ticks=0\n' >"$TEST_ROOT/repo/.artifacts/codex/state-reused/.meta"
printf '정상 로그\n' >"$TEST_ROOT/repo/.artifacts/codex/state-reused/job.log"
assert_success '재사용 pid 상태 조회 실패' job status state-reused
assert_file_contains "$TEST_ROOT/last.out" $'^state-reused\t.*\t죽음\t없음\t-$'
assert_failure '재사용 pid kill 거부 실패' 1 job kill state-reused
kill -0 "$$" 2>/dev/null || fail 'pid 재사용 검사에서 호출자 셸을 종료함'
pass '.meta와 /proc 시작 시각이 다르면 재사용된 pid로 보고 신호를 보내지 않는다'

printf '1..%s\n' "$PASS"
