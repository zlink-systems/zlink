#!/usr/bin/env bash

set -euo pipefail

TEST_ROOT=/tmp/zlink-sol-work-sh-cache/work-sh-test.$$
SOURCE_ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
WORK_SH="$SOURCE_ROOT/scripts/dev/work.sh"
PASS=0
export TEST_ROOT WORK_SH

cleanup() {
    [[ "$TEST_ROOT" == /tmp/zlink-sol-work-sh-cache/work-sh-test.* ]] && rm -rf -- "$TEST_ROOT"
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

assert_file_not_contains() {
    local file=$1 pattern=$2
    ! grep -Eq -- "$pattern" "$file" || fail "$file 에 없어야 하는 패턴이 있음: $pattern"
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

make_fake_gh() {
    mkdir -p "$TEST_ROOT/fake-bin" "$TEST_ROOT/gh-state"
    cat >"$TEST_ROOT/fake-bin/gh" <<'FAKE_GH'
#!/usr/bin/env bash
set -euo pipefail

state=${FAKE_GH_STATE:?}
printf '%q ' "$@" >>"$state/calls.log"
printf '\n' >>"$state/calls.log"

arg_value() {
    local wanted=$1
    shift
    while (($#)); do
        if [[ "$1" == "$wanted" && $# -ge 2 ]]; then printf '%s' "$2"; return 0; fi
        shift
    done
}

safe_branch() { printf '%s' "$1" | tr '/:' '__'; }
issue_title() {
    local number=$1 file="$state/issue-$number-title"
    if [[ -f "$file" ]]; then cat "$file"; else printf 'issue %s fixture' "$number"; fi
}

if [[ "${1:-}" == project && -f "$state/fail-project" ]]; then exit 1; fi

case "${1:-} ${2:-}" in
    'issue list')
        search=$(arg_value --search "$@" || true)
        if [[ -n "$search" ]]; then
            wanted=${search% in:title}
            if [[ -f "$state/created-number" ]]; then
                number=$(cat "$state/created-number")
                title=$(issue_title "$number")
                [[ "$title" == "$wanted" ]] && printf '%s\t%s\thttps://example.test/issues/%s\n' "$number" "$title" "$number"
            fi
        fi
        ;;
    'issue create')
        number=21
        title=$(arg_value --title "$@")
        printf '%s' "$number" >"$state/created-number"
        printf '%s' "$title" >"$state/issue-$number-title"
        printf 'area:ci\nkind:chore\n' >"$state/issue-$number-labels"
        printf 'https://example.test/issues/%s\n' "$number"
        ;;
    'issue view')
        number=$3
        field=$(arg_value --json "$@")
        case "$field" in
            title) issue_title "$number"; printf '\n' ;;
            url) printf 'https://example.test/issues/%s\n' "$number" ;;
            labels)
                if [[ -f "$state/issue-$number-labels" ]]; then cat "$state/issue-$number-labels"
                else printf 'area:ci\nkind:chore\n'; fi
                ;;
            milestone) [[ -f "$state/issue-$number-milestone" ]] && cat "$state/issue-$number-milestone" || true ;;
            state) [[ -f "$state/issue-$number-state" ]] && cat "$state/issue-$number-state" || printf 'OPEN\n' ;;
            *) exit 2 ;;
        esac
        ;;
    'issue edit')
        number=$3
        milestone=$(arg_value --milestone "$@" || true)
        [[ -z "$milestone" ]] || printf '%s\n' "$milestone" >"$state/issue-$number-milestone"
        ;;
    'project item-list')
        # 빈 목록이면 work.sh가 item-add의 반환 ID를 사용한다.
        ;;
    'project item-add') printf 'ITEM-1\n' ;;
    'project field-list')
        jq_filter=$(arg_value --jq "$@")
        if [[ "$jq_filter" == *options* ]]; then printf 'OPTION-1\n'; else printf 'FIELD-1\n'; fi
        ;;
    'project item-edit') ;;
    'pr list')
        branch=$(arg_value --head "$@" || true)
        key=$(safe_branch "$branch")
        requested_state=$(arg_value --state "$@" || true)
        if [[ -f "$state/pr-$key-number" ]]; then
            current=$(cat "$state/pr-$key-state")
            if [[ "$requested_state" == all || ("$requested_state" == open && "$current" == OPEN) ]]; then
                jq_filter=$(arg_value --jq "$@")
                if [[ "$jq_filter" == *'number // empty'* ]]; then cat "$state/pr-$key-number"
                elif [[ "$jq_filter" == *'.0.number'* ]]; then printf '#%s %s\n' "$(cat "$state/pr-$key-number")" "$current"
                fi
            fi
        fi
        ;;
    'pr create')
        branch=$(arg_value --head "$@")
        key=$(safe_branch "$branch")
        body=$(arg_value --body-file "$@")
        title=$(arg_value --title "$@")
        printf '7\n' >"$state/pr-$key-number"
        printf 'OPEN\n' >"$state/pr-$key-state"
        git rev-parse HEAD >"$state/pr-$key-head"
        cp "$body" "$state/pr-$key-body"
        printf '%s\n' "$title" >"$state/pr-$key-title"
        printf 'https://example.test/pull/7\n'
        ;;
    'pr edit')
        branch=$(git branch --show-current)
        key=$(safe_branch "$branch")
        body=$(arg_value --body-file "$@")
        title=$(arg_value --title "$@")
        cp "$body" "$state/pr-$key-body"
        printf '%s\n' "$title" >"$state/pr-$key-title"
        git rev-parse HEAD >"$state/pr-$key-head"
        ;;
    'pr view')
        branch=$(git branch --show-current)
        key=$(safe_branch "$branch")
        field=$(arg_value --json "$@")
        case "$field" in
            baseRefName) printf 'main\n' ;;
            url) printf 'https://example.test/pull/7\n' ;;
            state) cat "$state/pr-$key-state" ;;
            headRefOid) cat "$state/pr-$key-head" ;;
            body) sed -n '1p' "$state/pr-$key-body" ;;
            *) exit 2 ;;
        esac
        ;;
    'pr merge')
        branch=$(git branch --show-current)
        key=$(safe_branch "$branch")
        printf 'MERGED\n' >"$state/pr-$key-state"
        ;;
    'pr checks') printf 'PASS\n' ;;
    *) printf 'fake gh: 처리하지 못한 호출: %s\n' "$*" >&2; exit 2 ;;
esac
FAKE_GH
    chmod +x "$TEST_ROOT/fake-bin/gh"
}

make_repository() {
    mkdir -p "$TEST_ROOT/home/project" "$TEST_ROOT/baseline/.artifacts/wsl"
    git init --bare --initial-branch=main "$TEST_ROOT/remote.git" >/dev/null
    git init --initial-branch=main "$TEST_ROOT/seed" >/dev/null
    git -C "$TEST_ROOT/seed" config user.name tester
    git -C "$TEST_ROOT/seed" config user.email tester@example.test
    printf 'fixture\n' >"$TEST_ROOT/seed/README"
    mkdir -p "$TEST_ROOT/seed/scripts/local-package" "$TEST_ROOT/seed/scripts/dev"
    printf 'current_milestone=1.0\n' >"$TEST_ROOT/seed/scripts/dev/work.conf"
    cat >"$TEST_ROOT/seed/scripts/local-package/build-wsl.sh" <<'BUILD'
#!/usr/bin/env bash
set -euo pipefail
root=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
mkdir -p "$root/.artifacts/wsl/nuget"
printf 'prepared\n' >"$root/.artifacts/wsl/nuget/binding.fixture"
BUILD
    printf '.artifacts/\n' >"$TEST_ROOT/seed/.gitignore"
    git -C "$TEST_ROOT/seed" add README scripts .gitignore
    git -C "$TEST_ROOT/seed" commit -m 'seed: initial fixture' >/dev/null
    git -C "$TEST_ROOT/seed" remote add origin "$TEST_ROOT/remote.git"
    git -C "$TEST_ROOT/seed" push origin main >/dev/null
    git clone "$TEST_ROOT/remote.git" "$TEST_ROOT/repo" >/dev/null
    git -C "$TEST_ROOT/repo" config user.name tester
    git -C "$TEST_ROOT/repo" config user.email tester@example.test
}

work() {
    env HOME="$TEST_ROOT/home" \
        PATH="$TEST_ROOT/fake-bin:$PATH" \
        FAKE_GH_STATE="$TEST_ROOT/gh-state" \
        ZLINK_BASELINE_ROOT="$TEST_ROOT/baseline" \
        "$WORK_SH" "$@"
}

mkdir -p "$TEST_ROOT"
make_fake_gh
make_repository

cat >"$TEST_ROOT/issue.md" <<'EOF'
## 범위

work.sh fixture를 만든다.

## 완료 조건

테스트가 통과한다.

## 근거

Issue #20과 규격 문서.
EOF

cat >"$TEST_ROOT/bad-issue.md" <<'EOF'
## 범위

내용

## 완료 조건

## 근거

근거
EOF

assert_success '신규 Issue start 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f work); work start 'dev test automation' --area ci --kind chore --body '$TEST_ROOT/issue.md'"
[[ "$(cat "$TEST_ROOT/gh-state/created-number")" == 21 ]] || fail '신규 Issue 번호가 기록되지 않음'
WT21="$TEST_ROOT/home/project/zlink-21-dev-test-automation"
[[ -d "$WT21" ]] || fail '신규 worktree가 만들어지지 않음'
[[ -d "$WT21/.artifacts/wsl" && ! -L "$WT21/.artifacts/wsl" ]] || fail '패키지 루트가 독립 디렉터리가 아님'
[[ -f "$WT21/.artifacts/wsl/nuget/binding.fixture" ]] || fail '패키지 준비 스크립트가 실행되지 않음'
assert_file_contains "$TEST_ROOT/gh-state/calls.log" '^issue edit 21 --milestone 1.0 '
pass 'start가 신규 Issue·브랜치·worktree·패키지를 준비하고 기본 milestone을 설정한다'

create_calls_before=$(grep -c '^issue create ' "$TEST_ROOT/gh-state/calls.log")
assert_success 'start --issue 재실행 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f work); work start --issue 21"
create_calls_after=$(grep -c '^issue create ' "$TEST_ROOT/gh-state/calls.log")
[[ "$create_calls_before" -eq "$create_calls_after" ]] || fail '재실행에서 Issue가 중복 생성됨'
assert_file_contains "$TEST_ROOT/last.out" 'worktree 재사용'
[[ $(grep -c '^issue edit 21 --milestone 1.0 ' "$TEST_ROOT/gh-state/calls.log") -eq 1 ]] || fail '같은 milestone을 중복 설정함'
pass 'start --issue 재실행이 기존 Issue와 worktree를 재사용한다'

assert_failure 'Issue body 빈 항목 거부 실패' 2 bash -c "cd '$TEST_ROOT/repo' && $(declare -f work); work start 'bad body' --area ci --kind chore --body '$TEST_ROOT/bad-issue.md'"
assert_file_contains "$TEST_ROOT/last.err" '재개 명령:'
pass 'start가 빈 범위·완료 조건·근거를 거부한다'

printf 'change\n' >"$WT21/change.txt"
git -C "$WT21" add change.txt
git -C "$WT21" commit -m 'invalid title' >/dev/null
cat >"$TEST_ROOT/refs.md" <<'EOF'
Refs #21

## 변경

fixture

## 검증

테스트 실행.
EOF
assert_failure 'PR 제목 형식 거부 실패' 2 bash -c "cd '$WT21' && $(declare -f work); work pr --body '$TEST_ROOT/refs.md' --refs"
pass 'pr이 잘못된 제목 형식을 거부한다'

git -C "$WT21" commit --allow-empty -m 'dev: fixture change' >/dev/null
cat >"$TEST_ROOT/bad-pr.md" <<'EOF'
Closes #21

## 변경

fixture
EOF
assert_failure 'PR 본문 형식 거부 실패' 2 bash -c "cd '$WT21' && $(declare -f work); work pr --body '$TEST_ROOT/bad-pr.md' --refs"
pass 'pr이 첫 줄 참조와 검증 절을 검사한다'

assert_success 'PR 신규 생성 실패' bash -c "cd '$WT21' && $(declare -f work); work pr --body '$TEST_ROOT/refs.md' --refs"
assert_file_contains "$TEST_ROOT/gh-state/calls.log" '^pr create .*--base main .*--head ci/21-dev-test-automation'
printf '\n재실행 본문\n' >>"$TEST_ROOT/refs.md"
assert_success 'PR 재실행 갱신 실패' bash -c "cd '$WT21' && $(declare -f work); work pr --body '$TEST_ROOT/refs.md' --refs"
assert_file_contains "$TEST_ROOT/gh-state/calls.log" '^pr edit 7 .*--body-file'
pass 'pr 재실행이 기존 base=main PR을 갱신한다'

HEAD21=$(git -C "$WT21" rev-parse HEAD)
BAD_SHA=0000000000000000000000000000000000000000
assert_failure 'done SHA 불일치 거부 실패' 1 bash -c "cd '$WT21' && $(declare -f work); work done --verified '$BAD_SHA'"
assert_file_not_contains "$TEST_ROOT/gh-state/calls.log" "pr merge 7 --merge --match-head-commit $BAD_SHA"
pass 'done이 PR HEAD와 다른 검증 SHA를 거부한다'

assert_success 'Refs done 실패' bash -c "cd '$WT21' && $(declare -f work); work done --verified '$HEAD21'"
[[ -d "$WT21" ]] || fail 'Refs PR이 worktree를 제거함'
git --git-dir="$TEST_ROOT/remote.git" show-ref --verify --quiet refs/heads/ci/21-dev-test-automation \
    || fail 'Refs PR이 원격 브랜치를 삭제함'
assert_file_contains "$TEST_ROOT/last.out" 'Refs PR: Issue와 worktree를 유지'
pass 'done의 Refs 분기가 Issue·브랜치·worktree를 유지한다'

printf 'close cleanup fixture' >"$TEST_ROOT/gh-state/issue-22-title"
assert_success 'Closes fixture start 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f work); work start --issue 22 --no-packages"
assert_file_contains "$TEST_ROOT/gh-state/calls.log" '^issue edit 22 --milestone 1.0 '
WT22="$TEST_ROOT/home/project/zlink-22-close-cleanup-fixture"
[[ ! -e "$WT22/.artifacts/wsl" ]] || fail '--no-packages가 패키지를 준비함'
printf 'close\n' >"$WT22/close.txt"
git -C "$WT22" add close.txt
git -C "$WT22" commit -m 'dev: close fixture' >/dev/null
cat >"$TEST_ROOT/closes.md" <<'EOF'
Closes #22

## 변경

fixture

## 검증

테스트 실행.
EOF
assert_success 'Closes PR 생성 실패' bash -c "cd '$WT22' && $(declare -f work); work pr --body '$TEST_ROOT/closes.md' --closes"
HEAD22=$(git -C "$WT22" rev-parse HEAD)
printf 'dirty\n' >"$WT22/dirty.txt"
merge_calls_before=$(grep -c '^pr merge ' "$TEST_ROOT/gh-state/calls.log")
assert_failure 'dirty worktree 거부 실패' 1 bash -c "cd '$WT22' && $(declare -f work); work done --verified '$HEAD22'"
merge_calls_after=$(grep -c '^pr merge ' "$TEST_ROOT/gh-state/calls.log")
[[ "$merge_calls_before" -eq "$merge_calls_after" ]] || fail 'dirty worktree인데 PR을 merge함'
[[ -d "$WT22" ]] || fail 'dirty worktree를 제거함'
rm "$WT22/dirty.txt"
pass 'done이 dirty worktree 정리를 거부하고 상태를 보존한다'

assert_success 'Closes done 실패' bash -c "cd '$WT22' && $(declare -f work); work done --verified '$HEAD22'"
[[ ! -e "$WT22" ]] || fail 'Closes PR worktree가 남아 있음'
! git --git-dir="$TEST_ROOT/remote.git" show-ref --verify --quiet refs/heads/ci/22-close-cleanup-fixture \
    || fail 'Closes PR 원격 브랜치가 남아 있음'
assert_file_contains "$TEST_ROOT/gh-state/calls.log" '^project item-edit .*--single-select-option-id OPTION-1'
pass 'done의 Closes 분기가 merge·원격 브랜치 삭제·worktree 제거·Done 갱신을 수행한다'

printf 'project failure fixture' >"$TEST_ROOT/gh-state/issue-23-title"
touch "$TEST_ROOT/gh-state/fail-project"
assert_success 'Project 실패 best-effort 처리 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f work); work start --issue 23 --no-packages"
[[ -d "$TEST_ROOT/home/project/zlink-23-project-failure-fixture" ]] || fail 'Project 실패 뒤 로컬 worktree가 없음'
assert_file_contains "$TEST_ROOT/last.err" '^경고: Project'
rm "$TEST_ROOT/gh-state/fail-project"
pass 'Project 호출 실패가 로컬 start를 막지 않는다'

printf 'dry run fixture' >"$TEST_ROOT/gh-state/issue-24-title"
refs_before=$(git -C "$TEST_ROOT/repo" show-ref | sort)
worktrees_before=$(git -C "$TEST_ROOT/repo" worktree list --porcelain)
calls_before=$(wc -l <"$TEST_ROOT/gh-state/calls.log")
assert_success 'dry-run start 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f work); work --dry-run start --issue 24 --no-packages"
refs_after=$(git -C "$TEST_ROOT/repo" show-ref | sort)
worktrees_after=$(git -C "$TEST_ROOT/repo" worktree list --porcelain)
[[ "$refs_before" == "$refs_after" ]] || fail 'dry-run이 Git ref를 변경함'
[[ "$worktrees_before" == "$worktrees_after" ]] || fail 'dry-run이 worktree를 변경함'
[[ ! -e "$TEST_ROOT/home/project/zlink-24-dry-run-fixture" ]] || fail 'dry-run이 디렉터리를 만듦'
tail -n "+$((calls_before + 1))" "$TEST_ROOT/gh-state/calls.log" >"$TEST_ROOT/dry-run-calls.log"
assert_file_not_contains "$TEST_ROOT/dry-run-calls.log" '^(issue create|issue edit|project item-add|project item-edit|pr create|pr edit|pr merge) '
assert_file_contains "$TEST_ROOT/last.out" '^\[dry-run\] git fetch origin'
pass '--dry-run이 gh·git·worktree 상태를 바꾸지 않는다'

assert_file_not_contains "$TEST_ROOT/gh-state/calls.log" '^issue edit 24 '
assert_success '명시 milestone 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f work); work start 'explicit milestone' --area ci --kind chore --body '$TEST_ROOT/issue.md' --milestone 2.0 --no-packages"
assert_file_contains "$TEST_ROOT/gh-state/calls.log" '--milestone 2.0 '
pass '명시 milestone이 기본값보다 우선한다'

rm "$TEST_ROOT/repo/scripts/dev/work.conf"
calls_before=$(wc -l <"$TEST_ROOT/gh-state/calls.log")
assert_success '설정 없는 start 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f work); work start 'no milestone' --area ci --kind chore --body '$TEST_ROOT/issue.md' --no-packages"
tail -n "+$((calls_before + 1))" "$TEST_ROOT/gh-state/calls.log" >"$TEST_ROOT/no-conf-calls.log"
assert_file_not_contains "$TEST_ROOT/no-conf-calls.log" '--milestone'
pass 'work.conf가 없으면 milestone을 설정하지 않는다'

assert_success '패키지 dry-run 실패' bash -c "cd '$TEST_ROOT/repo' && $(declare -f work); work --dry-run start --issue 24"
assert_file_contains "$TEST_ROOT/last.out" '^\[dry-run\] bash .*scripts/local-package/build-wsl.sh'
[[ ! -e "$TEST_ROOT/home/project/zlink-24-dry-run-fixture" ]] || fail '패키지 dry-run이 디렉터리를 만듦'
pass '패키지 dry-run은 빌드 명령만 출력한다'

printf '1..%d\n' "$PASS"
