#!/usr/bin/env bash

set -euo pipefail

DRY_RUN=1
PRUNE=0
FORCE=0
EXPLICIT_DRY_RUN=0
RESUME_COMMAND=""
CONTROL_ROOT=""
CURRENT_ROOT=""
MAIN_REF=""
ACTIVE_UNKNOWN=0
declare -A ACTIVE_WORKTREES=()

usage() {
    cat <<'EOF'
사용법:
  worktree-sweep.sh [--dry-run]
  worktree-sweep.sh --prune [--dry-run] [--force]

옵션:
  --dry-run  worktree 판정만 출력합니다 (기본값).
  --prune    안전 조건을 모두 만족하는 worktree를 제거합니다.
  --force    untracked 파일 목록을 확인한 뒤 해당 worktree 제거를 허용합니다.
EOF
}

quote_command() {
    local quoted=() value
    for value in "$@"; do
        printf -v value '%q' "$value"
        quoted+=("$value")
    done
    printf '%s' "${quoted[*]}"
}

print_command() {
    printf '[dry-run] '
    quote_command "$@"
    printf '\n'
}

die() {
    local code=$1
    shift
    printf '오류: %s\n' "$*" >&2
    [[ -n "$RESUME_COMMAND" ]] && printf '재개 명령: %s\n' "$RESUME_COMMAND" >&2
    exit "$code"
}

on_error() {
    local code=$1 line=$2
    trap - ERR
    printf '오류: %s행에서 명령이 실패했습니다 (exit %s).\n' "$line" "$code" >&2
    [[ -n "$RESUME_COMMAND" ]] && printf '재개 명령: %s\n' "$RESUME_COMMAND" >&2
    exit "$code"
}
trap 'on_error $? $LINENO' ERR

require_command() {
    command -v "$1" >/dev/null 2>&1 || die 1 "필요한 명령을 찾을 수 없습니다: $1"
}

canonical_path() {
    readlink -f -- "$1" 2>/dev/null || printf '%s\n' "$1"
}

list_worktrees() {
    local path="" branch="(detached)" line
    while IFS= read -r line; do
        case "$line" in
            worktree\ *) path=${line#worktree }; branch='(detached)' ;;
            branch\ refs/heads/*) branch=${line#branch refs/heads/} ;;
            '')
                if [[ -n "$path" ]]; then
                    printf '%s\t%s\n' "$path" "$branch"
                fi
                path=''; branch='(detached)' ;;
        esac
    done < <(git -C "$CONTROL_ROOT" worktree list --porcelain; printf '\n')
}

select_main_ref() {
    if git -C "$CONTROL_ROOT" show-ref --verify --quiet refs/remotes/origin/main; then
        MAIN_REF=refs/remotes/origin/main
    elif git -C "$CONTROL_ROOT" show-ref --verify --quiet refs/heads/main; then
        MAIN_REF=refs/heads/main
    else
        MAIN_REF=""
    fi
}

status_count() {
    local path=$1 mode=$2 output
    if [[ "$mode" == tracked ]]; then
        output=$(git -C "$path" status --porcelain=v1 --untracked-files=no)
    else
        output=$(git -C "$path" ls-files --others --exclude-standard)
    fi
    if [[ -z "$output" ]]; then
        printf '0\n'
    else
        printf '%s\n' "$output" | wc -l | tr -d '[:space:]'
        printf '\n'
    fi
}

unpushed_count() {
    git -C "$1" rev-list --count HEAD --not --remotes
}

main_inclusion() {
    local path=$1
    if [[ -z "$MAIN_REF" ]]; then
        printf '확인 불가\n'
    elif git -C "$path" merge-base --is-ancestor HEAD "$MAIN_REF"; then
        printf '예\n'
    else
        printf '아니요\n'
    fi
}

disk_size() {
    local output
    if output=$(du -sh -- "$1" 2>/dev/null); then
        printf '%s\n' "${output%%[[:space:]]*}"
    else
        printf '?\n'
    fi
}

last_commit_date() {
    local date
    if date=$(git -C "$1" log -1 --format=%cs 2>/dev/null) && [[ -n "$date" ]]; then
        printf '%s\n' "$date"
    else
        printf -- '-\n'
    fi
}

path_owner_worktree() {
    local cwd=$1 path _branch canonical
    while IFS=$'\t' read -r path _branch; do
        canonical=$(canonical_path "$path")
        if [[ "$cwd" == "$canonical" || "$cwd" == "$canonical/"* ]]; then
            printf '%s\n' "$canonical"
            return 0
        fi
    done < <(list_worktrees)
    return 1
}


# 실행 중 job이 선언한 작업 worktree. .meta의 기록을 먼저 보고, 없으면 명령줄의 -C 인자를 읽는다.
job_declared_worktree() {
    local pidfile=$1 pid=$2 meta value previous item
    meta="${pidfile%/.pid}/.meta"
    if [[ -f "$meta" ]]; then
        value=$(sed -n 's/^worktree=//p' "$meta" | head -n 1)
        if [[ -n "$value" ]]; then
            readlink -f -- "$value" 2>/dev/null || printf '%s' "$value"
            return 0
        fi
    fi
    [[ -r "/proc/$pid/cmdline" ]] || return 0
    previous=""
    while IFS= read -r -d '' item; do
        if [[ "$previous" == "-C" || "$previous" == "--cd" ]]; then
            readlink -f -- "$item" 2>/dev/null || printf '%s' "$item"
            return 0
        fi
        previous=$item
    done < "/proc/$pid/cmdline"
    return 0
}

load_active_worktrees() {
    local path _branch pidfile pid cwd owner declared
    ACTIVE_WORKTREES=()
    ACTIVE_UNKNOWN=0
    while IFS=$'\t' read -r path _branch; do
        for pidfile in "$path"/.artifacts/codex/*/.pid; do
            [[ -f "$pidfile" ]] || continue
            pid=$(tr -d '[:space:]' <"$pidfile")
            [[ "$pid" =~ ^[0-9]+$ ]] || continue
            kill -0 "$pid" 2>/dev/null || continue
            # job이 실제로 고치는 곳은 실행 디렉터리가 아니라 codex에 준 worktree다.
            # codex는 launch 디렉터리에서 돌기 때문에 cwd만 보면 작업 worktree를 보호하지 못한다.
            declared=$(job_declared_worktree "$pidfile" "$pid")
            if [[ -n "$declared" ]] && owner=$(path_owner_worktree "$declared"); then
                ACTIVE_WORKTREES["$owner"]=1
            fi
            if ! cwd=$(readlink -f -- "/proc/$pid/cwd" 2>/dev/null); then
                ACTIVE_UNKNOWN=1
                printf '경고: 실행 중 PID %s의 작업 경로를 읽지 못해 모든 삭제를 보호합니다: %s\n' \
                    "$pid" "$pidfile" >&2
                continue
            fi
            if owner=$(path_owner_worktree "$cwd"); then
                ACTIVE_WORKTREES["$owner"]=1
            fi
        done
    done < <(list_worktrees)
}

print_table() {
    local path branch tracked untracked dirty unpushed included size date
    printf '경로\t브랜치\t커밋되지 않은 변경 수\tpush되지 않은 커밋 수\tmain 포함\t디스크 크기\t마지막 커밋 날짜\n'
    while IFS=$'\t' read -r path branch; do
        if [[ ! -d "$path" ]]; then
            printf '%s\t%s\t-\t-\t확인 불가\t-\t-\n' "$path" "$branch"
            continue
        fi
        tracked=$(status_count "$path" tracked)
        untracked=$(status_count "$path" untracked)
        dirty=$((tracked + untracked))
        unpushed=$(unpushed_count "$path")
        included=$(main_inclusion "$path")
        size=$(disk_size "$path")
        date=$(last_commit_date "$path")
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
            "$path" "$branch" "$dirty" "$unpushed" "$included" "$size" "$date"
    done < <(list_worktrees)
}

print_untracked_files() {
    local path=$1 file
    printf 'untracked 파일 (%s):\n' "$path"
    while IFS= read -r -d '' file; do
        printf '  - %q\n' "$file"
    done < <(git -C "$path" ls-files --others --exclude-standard -z)
}

append_reason() {
    local -n target=$1
    local reason=$2
    if [[ -n "$target" ]]; then
        target+="; $reason"
    else
        target=$reason
    fi
}

prune_one() {
    local path=$1 branch=$2 canonical tracked untracked unpushed included reasons=""
    canonical=$(canonical_path "$path")
    if [[ ! -d "$path" ]]; then
        printf '건너뜀: %s (worktree 경로가 없습니다)\n' "$path"
        return 0
    fi

    # 표를 만든 뒤 상태가 바뀌었을 수 있으므로 제거 직전에 전부 다시 판정한다.
    load_active_worktrees
    tracked=$(status_count "$path" tracked)
    untracked=$(status_count "$path" untracked)
    unpushed=$(unpushed_count "$path")
    included=$(main_inclusion "$path")

    [[ "$canonical" != "$(canonical_path "$CONTROL_ROOT")" ]] \
        || append_reason reasons '기본 worktree'
    [[ "$canonical" != "$(canonical_path "$CURRENT_ROOT")" ]] \
        || append_reason reasons '현재 스크립트를 실행한 worktree'
    ((ACTIVE_UNKNOWN == 0)) || append_reason reasons '작업 경로를 확인할 수 없는 실행 중 job'
    [[ -z "${ACTIVE_WORKTREES[$canonical]:-}" ]] || append_reason reasons '실행 중 job'
    ((tracked == 0)) || append_reason reasons "tracked 변경 $tracked개"
    ((unpushed == 0)) || append_reason reasons "push되지 않은 커밋 $unpushed개"
    if [[ "$branch" != '(detached)' && "$included" != '예' ]]; then
        append_reason reasons 'main에 포함되지 않은 브랜치'
    fi
    if ((untracked > 0)); then
        print_untracked_files "$path"
        ((FORCE)) || append_reason reasons "untracked 파일 $untracked개(--force 필요)"
    fi

    if [[ -n "$reasons" ]]; then
        printf '건너뜀: %s (%s)\n' "$path" "$reasons"
        return 0
    fi

    if ((DRY_RUN)); then
        if ((untracked > 0)); then
            print_command git -C "$CONTROL_ROOT" worktree remove --force "$path"
        else
            print_command git -C "$CONTROL_ROOT" worktree remove "$path"
        fi
        return 0
    fi
    if ((untracked > 0)); then
        git -C "$CONTROL_ROOT" worktree remove --force "$path"
    else
        git -C "$CONTROL_ROOT" worktree remove "$path"
    fi
    printf '제거 완료: %s\n' "$path"
}

prune_worktrees() {
    local path branch
    while IFS=$'\t' read -r path branch; do
        prune_one "$path" "$branch"
    done < <(list_worktrees)
}

main() {
    local original=("$0" "$@") arg
    RESUME_COMMAND=$(quote_command "${original[@]}")
    require_command git
    require_command du
    require_command readlink

    for arg in "$@"; do
        case "$arg" in
            --dry-run) EXPLICIT_DRY_RUN=1 ;;
            --prune) PRUNE=1 ;;
            --force) FORCE=1 ;;
            -h|--help) usage; return 0 ;;
            *) usage; die 2 "알 수 없는 인자입니다: $arg" ;;
        esac
    done
    ((FORCE == 0 || PRUNE == 1)) || die 2 "--force는 --prune과 함께 사용해야 합니다."
    if ((PRUNE == 1 && EXPLICIT_DRY_RUN == 0)); then
        DRY_RUN=0
    fi

    CURRENT_ROOT=$(git rev-parse --show-toplevel 2>/dev/null) \
        || die 1 "Git 저장소 안에서 실행해야 합니다."
    CONTROL_ROOT=$(git -C "$CURRENT_ROOT" worktree list --porcelain \
        | awk '$1 == "worktree" { print substr($0, index($0, " ") + 1); exit }')
    [[ -n "$CONTROL_ROOT" ]] || die 1 "기본 worktree를 찾지 못했습니다."
    select_main_ref
    load_active_worktrees
    print_table
    if ((PRUNE)); then
        printf '\n정리 판정\n'
        prune_worktrees
    fi
}

main "$@"
