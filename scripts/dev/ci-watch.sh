#!/usr/bin/env bash

set -euo pipefail

DRY_RUN=0
INTERVAL_SECONDS=600
STATE_DIR="${ZLINK_CI_WATCH_STATE_DIR:-${XDG_RUNTIME_DIR:-/tmp}/zlink-ci-watch-${UID}}"
LOCK_FILE="$STATE_DIR/watcher.lock"
PID_FILE="$STATE_DIR/watcher.pid"
TARGET_FILE="$STATE_DIR/target"
TOKEN_FILE="$STATE_DIR/token"
LAST_STATE_FILE="$STATE_DIR/last-state"
LOG_FILE="$STATE_DIR/ci-watch.log"
SCRIPT_PATH=$(cd "$(dirname "$0")" && pwd -P)/$(basename "$0")
RESUME_COMMAND=""

usage() {
    cat <<'EOF'
사용법:
  ci-watch.sh [--dry-run] start (--pr <번호> | --branch <이름>)
  ci-watch.sh [--dry-run] status
  ci-watch.sh [--dry-run] stop
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

require_repository() {
    git rev-parse --show-toplevel >/dev/null 2>&1 || die 1 "Git 저장소 안에서 실행해야 합니다."
}

ensure_state_dir() {
    umask 077
    mkdir -p -- "$STATE_DIR"
}

validate_target() {
    local kind=$1 value=$2
    case "$kind" in
        pr)
            [[ "$value" =~ ^[1-9][0-9]*$ ]] || die 2 "PR 번호는 1 이상의 정수여야 합니다."
            ;;
        branch)
            [[ -n "$value" && "$value" != -* && "$value" != *$'\n'* ]] || die 2 "브랜치 이름이 올바르지 않습니다."
            ;;
        *) die 2 "감시 대상이 올바르지 않습니다." ;;
    esac
}

watcher_is_active() {
    ! flock -n "$LOCK_FILE" true
}

clear_stale_state() {
    rm -f -- "$PID_FILE" "$TARGET_FILE" "$TOKEN_FILE" "$LAST_STATE_FILE"
    : >"$LOG_FILE"
}

read_file() {
    [[ -f "$1" ]] && IFS= read -r REPLY <"$1" || REPLY=""
}

format_target() {
    local kind=$1 value=$2
    if [[ "$kind" == pr ]]; then
        printf 'PR #%s' "$value"
    else
        printf '브랜치 %s' "$value"
    fi
}

make_resume_command() {
    local action=$1
    shift
    quote_command "$SCRIPT_PATH" "$action" "$@"
}

parse_target_arguments() {
    TARGET_KIND=""
    TARGET_VALUE=""
    while (($#)); do
        case "$1" in
            --pr)
                [[ $# -ge 2 && -z "$TARGET_KIND" ]] || die 2 "--pr 또는 --branch 중 하나만 지정해야 합니다."
                TARGET_KIND='pr'
                TARGET_VALUE=$2
                shift 2
                ;;
            --branch)
                [[ $# -ge 2 && -z "$TARGET_KIND" ]] || die 2 "--pr 또는 --branch 중 하나만 지정해야 합니다."
                TARGET_KIND='branch'
                TARGET_VALUE=$2
                shift 2
                ;;
            *) die 2 "알 수 없는 인자입니다: $1" ;;
        esac
    done
    [[ -n "$TARGET_KIND" ]] || die 2 "--pr 또는 --branch를 지정해야 합니다."
    validate_target "$TARGET_KIND" "$TARGET_VALUE"
}

write_state_change() {
    local state=$1
    local checksum
    checksum=$(printf '%s' "$state" | cksum)
    read_file "$LAST_STATE_FILE"
    [[ "$REPLY" == "$checksum" ]] && return 0

    printf '%s\n' "$checksum" >"$LAST_STATE_FILE"
    printf '[%(%Y-%m-%dT%H:%M:%S%z)T] CI 상태 변경: %s\n' -1 "$(format_target "$WORKER_KIND" "$WORKER_VALUE")"
    printf '%s\n' "$state"
}

fetch_ci_state() {
    local output code
    set +e
    if [[ "$WORKER_KIND" == pr ]]; then
        output=$(gh pr checks "$WORKER_VALUE" 2>&1)
        code=$?
    else
        output=$(gh run list --branch "$WORKER_VALUE" --limit 1 \
            --json databaseId,status,conclusion,workflowName,headSha,url 2>&1)
        code=$?
    fi
    set -e

    # gh pr checks는 실패한 check를 정상 응답으로 돌려준 뒤 exit 8을 쓴다.
    if ((code != 0)) && ! [[ "$WORKER_KIND" == pr && "$code" -eq 8 ]]; then
        FETCH_ERROR=$output
        return 1
    fi
    FETCH_STATE=$output
    return 0
}

cleanup_worker() {
    local recorded_pid
    trap - EXIT INT TERM
    read_file "$PID_FILE"
    recorded_pid=$REPLY
    if [[ "$recorded_pid" == "$$" ]]; then
        rm -f -- "$PID_FILE" "$TARGET_FILE" "$TOKEN_FILE" "$LAST_STATE_FILE"
    fi
}

stop_worker() {
    cleanup_worker
    exit 0
}

run_worker() {
    [[ $# -eq 3 ]] || exit 2
    WORKER_KIND=$1
    WORKER_VALUE=$2
    local token=$3

    validate_target "$WORKER_KIND" "$WORKER_VALUE"
    ensure_state_dir
    exec 9>>"$LOCK_FILE"
    flock -n 9 || exit 0

    printf '%s\n' "$$" >"$PID_FILE"
    printf '%s\t%s\n' "$WORKER_KIND" "$WORKER_VALUE" >"$TARGET_FILE"
    printf '%s\n' "$token" >"$TOKEN_FILE"
    trap cleanup_worker EXIT
    trap stop_worker INT TERM

    while :; do
        if fetch_ci_state; then
            write_state_change "$FETCH_STATE"
        else
            write_state_change "CI 상태 조회 실패: $FETCH_ERROR"$'\n'"재개 명령: $(make_resume_command start "--$WORKER_KIND" "$WORKER_VALUE")"
        fi
        # sleep 자식이 lock FD를 물려받으면 stop 뒤에도 다음 주기까지 lock이 남는다.
        sleep "$INTERVAL_SECONDS" 9>&-
    done
}

start_watcher() {
    local token worker_pid attempt
    RESUME_COMMAND=$(make_resume_command start)
    parse_target_arguments "$@"
    RESUME_COMMAND=$(make_resume_command start "--$TARGET_KIND" "$TARGET_VALUE")
    require_command git
    require_command gh
    require_command flock
    require_repository

    if ((DRY_RUN)); then
        printf '[dry-run] %s를 600초 주기 CI 감시자로 시작합니다.\n' "$(format_target "$TARGET_KIND" "$TARGET_VALUE")"
        return 0
    fi

    ensure_state_dir
    if watcher_is_active; then
        RESUME_COMMAND=$(make_resume_command status)
        die 1 "이미 실행 중인 CI 감시자가 있습니다."
    fi
    clear_stale_state
    token="$$-$(date +%s)"
    "$SCRIPT_PATH" --worker "$TARGET_KIND" "$TARGET_VALUE" "$token" >>"$LOG_FILE" 2>&1 &
    worker_pid=$!

    for ((attempt = 0; attempt < 20; attempt++)); do
        read_file "$TOKEN_FILE"
        if [[ "$REPLY" == "$token" ]]; then
            printf 'CI 감시자를 시작했습니다: %s (10분 주기)\n' "$(format_target "$TARGET_KIND" "$TARGET_VALUE")"
            printf '상태 변화 로그: %s\n' "$LOG_FILE"
            return 0
        fi
        if ! kill -0 "$worker_pid" >/dev/null 2>&1; then
            printf '감시자 로그 마지막 내용:\n' >&2
            tail -n 20 "$LOG_FILE" >&2 || true
            die 1 "CI 감시자를 시작하지 못했습니다."
        fi
        sleep 0.1
    done
    die 1 "CI 감시자의 시작 확인 시간이 초과되었습니다."
}

status_watcher() {
    local target pid
    RESUME_COMMAND=$(make_resume_command status)
    (($# == 0)) || die 2 "status는 추가 인자를 받지 않습니다."
    require_command flock
    ensure_state_dir
    if ! watcher_is_active; then
        printf '실행 중인 CI 감시자가 없습니다.\n'
        return 0
    fi
    read_file "$TARGET_FILE"
    target=$REPLY
    read_file "$PID_FILE"
    pid=$REPLY
    printf 'CI 감시자 실행 중: %s (pid %s, 10분 주기)\n' "${target:-대상 확인 중}" "${pid:-확인 중}"
    printf '상태 변화 로그: %s\n' "$LOG_FILE"
}

stop_watcher() {
    local pid
    RESUME_COMMAND=$(make_resume_command stop)
    (($# == 0)) || die 2 "stop은 추가 인자를 받지 않습니다."
    require_command flock
    ensure_state_dir
    if ! watcher_is_active; then
        printf '실행 중인 CI 감시자가 없습니다.\n'
        return 0
    fi
    read_file "$PID_FILE"
    pid=$REPLY
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || die 1 "기록한 감시자 pid를 읽지 못했습니다."
    if ! kill -0 "$pid" >/dev/null 2>&1; then
        die 1 "기록한 감시자 pid $pid가 더는 실행 중이 아닙니다. 재실행하세요."
    fi
    kill -TERM "$pid"
    printf 'CI 감시자 종료를 요청했습니다 (pid %s).\n' "$pid"
}

main() {
    local args=() argument command
    RESUME_COMMAND=$(make_resume_command --help)
    for argument in "$@"; do
        if [[ "$argument" == --dry-run ]]; then
            DRY_RUN=1
        else
            args+=("$argument")
        fi
    done
    set -- "${args[@]}"
    command=${1:-}
    [[ -n "$command" ]] || { usage >&2; die 2 "명령을 지정해야 합니다."; }
    shift
    case "$command" in
        start) start_watcher "$@" ;;
        status) status_watcher "$@" ;;
        stop) stop_watcher "$@" ;;
        --worker) ((DRY_RUN == 0)) || exit 2; run_worker "$@" ;;
        --help|-h) usage ;;
        *) usage >&2; die 2 "알 수 없는 명령입니다: $command" ;;
    esac
}

main "$@"
