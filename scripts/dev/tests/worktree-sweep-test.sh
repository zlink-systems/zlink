#!/usr/bin/env bash

set -euo pipefail

TEST_ROOT=/tmp/zlink-worktree-sweep-test.$$
SOURCE_ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
SWEEP_SH="$SOURCE_ROOT/scripts/dev/worktree-sweep.sh"
PASS=0
RUN_PID=""
export TEST_ROOT SWEEP_SH

cleanup() {
    if [[ -n "$RUN_PID" ]]; then
        kill "$RUN_PID" 2>/dev/null || true
        wait "$RUN_PID" 2>/dev/null || true
    fi
    [[ "$TEST_ROOT" == /tmp/zlink-worktree-sweep-test.* ]] && rm -rf -- "$TEST_ROOT"
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
    grep -Eq -- "$pattern" "$file" || fail "$file 에서 패턴을 찾지 못함: $pattern"
}

assert_success() {
    local description=$1
    shift
    if ! "$@" >"$TEST_ROOT/last.out" 2>"$TEST_ROOT/last.err"; then
        sed -n '1,200p' "$TEST_ROOT/last.out" >&2
        sed -n '1,200p' "$TEST_ROOT/last.err" >&2
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

make_repository() {
    mkdir -p "$TEST_ROOT/fake-bin"
    git init --bare --initial-branch=main "$TEST_ROOT/remote.git" >/dev/null
    git init --initial-branch=main "$TEST_ROOT/seed" >/dev/null
    git -C "$TEST_ROOT/seed" config user.name tester
    git -C "$TEST_ROOT/seed" config user.email tester@example.test
    printf 'fixture\n' >"$TEST_ROOT/seed/tracked.txt"
    printf '.artifacts/\n' >"$TEST_ROOT/seed/.gitignore"
    git -C "$TEST_ROOT/seed" add tracked.txt .gitignore
    git -C "$TEST_ROOT/seed" commit -m 'seed fixture' >/dev/null
    git -C "$TEST_ROOT/seed" remote add origin "$TEST_ROOT/remote.git"
    git -C "$TEST_ROOT/seed" push origin main >/dev/null
    git clone "$TEST_ROOT/remote.git" "$TEST_ROOT/repo" >/dev/null
    git -C "$TEST_ROOT/repo" config user.name tester
    git -C "$TEST_ROOT/repo" config user.email tester@example.test

    git -C "$TEST_ROOT/repo" worktree add -b dirty "$TEST_ROOT/wt-dirty" origin/main >/dev/null
    printf 'dirty\n' >>"$TEST_ROOT/wt-dirty/tracked.txt"

    git -C "$TEST_ROOT/repo" worktree add -b unpushed "$TEST_ROOT/wt-unpushed" origin/main >/dev/null
    printf 'local\n' >>"$TEST_ROOT/wt-unpushed/tracked.txt"
    git -C "$TEST_ROOT/wt-unpushed" add tracked.txt
    git -C "$TEST_ROOT/wt-unpushed" commit -m 'local only' >/dev/null

    git -C "$TEST_ROOT/repo" worktree add -b merged "$TEST_ROOT/wt-merged" origin/main >/dev/null
    printf 'merged\n' >"$TEST_ROOT/wt-merged/merged.txt"
    git -C "$TEST_ROOT/wt-merged" add merged.txt
    git -C "$TEST_ROOT/wt-merged" commit -m 'merged change' >/dev/null
    git -C "$TEST_ROOT/wt-merged" push -u origin merged >/dev/null
    git -C "$TEST_ROOT/repo" merge --no-edit merged >/dev/null
    git -C "$TEST_ROOT/repo" push origin main >/dev/null

    git -C "$TEST_ROOT/repo" worktree add --detach "$TEST_ROOT/wt-detached" origin/main >/dev/null
    git -C "$TEST_ROOT/repo" worktree add -b running "$TEST_ROOT/wt-running" origin/main >/dev/null
    mkdir -p "$TEST_ROOT/repo/.artifacts/codex/running"
    (cd "$TEST_ROOT/wt-running" && printf 'ready\n' >"$TEST_ROOT/running.ready" && exec sleep 300) &
    RUN_PID=$!
    while [[ ! -f "$TEST_ROOT/running.ready" ]]; do
        :
    done
    printf '%s\n' "$RUN_PID" >"$TEST_ROOT/repo/.artifacts/codex/running/.pid"

    git -C "$TEST_ROOT/repo" worktree add -b untracked "$TEST_ROOT/wt-untracked" origin/main >/dev/null
    printf 'keep me\n' >"$TEST_ROOT/wt-untracked/untracked.txt"

    cat >"$TEST_ROOT/fake-bin/du" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
path=${!#}
printf '42K\t%s\n' "$path"
EOF
    chmod +x "$TEST_ROOT/fake-bin/du"
}

sweep() {
    env PATH="$TEST_ROOT/fake-bin:$PATH" "$SWEEP_SH" "$@"
}

mkdir -p "$TEST_ROOT"
make_repository

assert_success '기본 dry-run 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f sweep); sweep"
assert_contains "$TEST_ROOT/last.out" $'^경로\t브랜치\t커밋되지 않은 변경 수\tpush되지 않은 커밋 수\tmain 포함\t디스크 크기\t마지막 커밋 날짜$'
assert_contains "$TEST_ROOT/last.out" "$TEST_ROOT/wt-dirty.*dirty.*1.*0.*예.*42K.*[0-9]{4}-[0-9]{2}-[0-9]{2}"
assert_contains "$TEST_ROOT/last.out" "$TEST_ROOT/wt-unpushed.*unpushed.*0.*1.*아니요.*42K"
assert_contains "$TEST_ROOT/last.out" "$TEST_ROOT/wt-merged.*merged.*0.*0.*예.*42K"
assert_contains "$TEST_ROOT/last.out" "$TEST_ROOT/wt-detached.*\\(detached\\).*0.*0.*예.*42K"
[[ -d "$TEST_ROOT/wt-merged" && -d "$TEST_ROOT/wt-detached" ]] || fail 'dry-run이 worktree를 제거함'
pass '기본 dry-run이 모든 worktree의 7개 상태 열을 표로 출력하고 변경하지 않는다'

assert_success '명시적 --dry-run 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f sweep); sweep --prune --dry-run"
[[ -d "$TEST_ROOT/wt-merged" && -d "$TEST_ROOT/wt-detached" ]] || fail '--dry-run이 제거 후보를 실제로 제거함'
assert_contains "$TEST_ROOT/last.out" "^\\[dry-run\\] git -C .* worktree remove $TEST_ROOT/wt-merged$"
pass '--dry-run은 --prune과 함께 사용해도 제거 명령만 보여 준다'

assert_success '--prune 판정 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f sweep); sweep --prune"
[[ ! -e "$TEST_ROOT/wt-merged" ]] || fail 'merged worktree가 제거되지 않음'
[[ ! -e "$TEST_ROOT/wt-detached" ]] || fail 'detached worktree가 제거되지 않음'
[[ -d "$TEST_ROOT/wt-dirty" ]] || fail 'dirty worktree가 제거됨'
[[ -d "$TEST_ROOT/wt-unpushed" ]] || fail '미push worktree가 제거됨'
[[ -d "$TEST_ROOT/wt-running" ]] || fail '실행 중 worktree가 제거됨'
assert_contains "$TEST_ROOT/last.out" "건너뜀: $TEST_ROOT/wt-dirty .*tracked 변경 1개"
assert_contains "$TEST_ROOT/last.out" "건너뜀: $TEST_ROOT/wt-unpushed .*push되지 않은 커밋 1개"
assert_contains "$TEST_ROOT/last.out" "건너뜀: $TEST_ROOT/wt-running .*실행 중 job"
pass '--prune이 merged·detached만 지우고 dirty·미push·실행 중 worktree를 이유와 함께 건너뛴다'

[[ -d "$TEST_ROOT/wt-untracked" ]] || fail '--force 없이 untracked worktree가 제거됨'
assert_contains "$TEST_ROOT/last.out" "untracked 파일 \($TEST_ROOT/wt-untracked\):"
assert_contains "$TEST_ROOT/last.out" '^  - untracked.txt$'
assert_contains "$TEST_ROOT/last.out" 'untracked 파일 1개\(--force 필요\)'
pass '--force가 없으면 untracked 파일 목록을 보이고 제거하지 않는다'

assert_success '--force 제거 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f sweep); sweep --prune --force"
[[ ! -e "$TEST_ROOT/wt-untracked" ]] || fail '--force로 untracked worktree를 제거하지 못함'
[[ -d "$TEST_ROOT/wt-dirty" ]] || fail '--force가 tracked 변경까지 제거함'
assert_contains "$TEST_ROOT/last.out" "untracked 파일 \($TEST_ROOT/wt-untracked\):"
assert_contains "$TEST_ROOT/last.out" "제거 완료: $TEST_ROOT/wt-untracked"
pass '--force는 목록을 출력한 untracked 파일만 허용하고 tracked 변경 보호는 유지한다'

assert_failure '잘못된 인자 거부 실패' 2 bash -c "cd '$TEST_ROOT/repo' && $(declare -f sweep); sweep --unknown"
assert_contains "$TEST_ROOT/last.err" '재개 명령:'
pass '잘못된 인자는 exit 2와 재개 명령을 출력한다'

printf '1..%d\n' "$PASS"
