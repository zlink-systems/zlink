#!/usr/bin/env bash

set -euo pipefail

DRY_RUN=0
RESUME_COMMAND=""
ARTIFACT_ROOT=""
STARTING_PID=""
STARTING_TICKS=""
STATUS_ALL=0
# job은 저장소를 고쳐야 하므로 기본은 승인·샌드박스 없이 실행한다.
# 읽기 전용 조사에는 --read-only를 준다.
SANDBOX_ARGS=(--dangerously-bypass-approvals-and-sandbox)
# 로그 본문(코드 인용 등)의 우연한 일치를 막으려고 줄 머리의 오류 표시나 고유 문구만 본다.
ERROR_PATTERN='^(ERROR|error)[: ]|^codex: |invalid_request_error|not supported when using Codex|(^|[[:space:]])(401|403) ?(Forbidden|forbidden|Unauthorized|unauthorized)|rate limit exceeded|No space left on device|panicked at|command not found'

usage() {
    cat <<'EOF'
사용법:
  job.sh [--dry-run] start <이름> --worktree <경로> --brief <파일> [--read-only] [--model <모델>] [--effort high|medium|low] [--max-jobs N]
  job.sh [--dry-run] status [--all] [<이름>]
  job.sh [--dry-run] watch [--interval 180]
  job.sh [--dry-run] kill <이름>
  job.sh [--dry-run] logs <이름> [-n N]
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

state_summary() {
    [[ -n "$ARTIFACT_ROOT" ]] && printf '  job 디렉터리: %s\n' "$ARTIFACT_ROOT" >&2
    return 0
}

die() {
    local code=$1
    shift
    printf '오류: %s\n' "$*" >&2
    state_summary
    [[ -n "$RESUME_COMMAND" ]] && printf '재개 명령: %s\n' "$RESUME_COMMAND" >&2
    exit "$code"
}

on_error() {
    local code=$1 line=$2
    trap - ERR
    printf '오류: %s행에서 명령이 실패했습니다 (exit %s).\n' "$line" "$code" >&2
    state_summary
    [[ -n "$RESUME_COMMAND" ]] && printf '재개 명령: %s\n' "$RESUME_COMMAND" >&2
    exit "$code"
}
trap 'on_error $? $LINENO' ERR

require_command() {
    command -v "$1" >/dev/null 2>&1 || die 1 "필요한 명령을 찾을 수 없습니다: $1"
}


primary_worktree() {
    local first
    first=$(git worktree list --porcelain 2>/dev/null \
        | awk '$1 == "worktree" { print substr($0, index($0, " ") + 1); exit }')
    if [[ -n "$first" ]]; then
        printf '%s' "$first"
    else
        printf '%s' "$(repo_root)"
    fi
}

repo_root() {
    git rev-parse --show-toplevel 2>/dev/null || die 1 "Git 저장소 안에서 실행해야 합니다."
}

validate_name() {
    [[ "$1" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] \
        || die 2 "job 이름은 영문자·숫자로 시작하고 영문자·숫자·점·밑줄·하이픈만 쓸 수 있습니다: $1"
}

validate_positive_integer() {
    local label=$1 value=$2
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || die 2 "$label 값은 양의 정수여야 합니다: $value"
}

meta_value() {
    local file=$1 key=$2 line
    [[ -f "$file" ]] || return 0
    line=$(grep -m 1 -F "${key}=" "$file" 2>/dev/null || true)
    [[ "$line" == "$key="* ]] && printf '%s' "${line#*=}"
    return 0
}

proc_start_ticks() {
    local pid=$1 stat rest
    local -a fields=()
    [[ -r "/proc/$pid/stat" ]] || return 0
    stat=$(<"/proc/$pid/stat")
    rest=${stat##*) }
    read -r -a fields <<<"$rest"
    ((${#fields[@]} >= 20)) && printf '%s' "${fields[19]}"
    return 0
}

pid_matches() {
    local pid=$1 expected_ticks=${2:-} actual_ticks
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    if [[ -n "$expected_ticks" ]]; then
        actual_ticks=$(proc_start_ticks "$pid")
        [[ -z "$actual_ticks" || "$actual_ticks" == "$expected_ticks" ]] || return 1
    fi
}

job_pid() {
    local job_dir=$1 pid
    [[ -f "$job_dir/.pid" ]] || return 1
    IFS= read -r pid <"$job_dir/.pid" || true
    [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
    printf '%s' "$pid"
}

job_is_running() {
    local job_dir=$1 pid expected_ticks
    pid=$(job_pid "$job_dir") || return 1
    expected_ticks=$(meta_value "$job_dir/.meta" proc_start_ticks)
    pid_matches "$pid" "$expected_ticks"
}

stop_pid() {
    local pid=$1 expected_ticks=${2:-} attempt
    pid_matches "$pid" "$expected_ticks" || return 0
    kill -TERM "$pid" 2>/dev/null || true
    for ((attempt = 0; attempt < 10; attempt++)); do
        pid_matches "$pid" "$expected_ticks" || return 0
        sleep 0.5
    done
    if pid_matches "$pid" "$expected_ticks"; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
}

cleanup_starting_process() {
    if [[ -n "$STARTING_PID" ]]; then
        stop_pid "$STARTING_PID" "$STARTING_TICKS"
        STARTING_PID=""
        STARTING_TICKS=""
    fi
}
trap cleanup_starting_process EXIT

log_has_error() {
    local log=$1
    [[ -f "$log" ]] && grep -Eqi -- "$ERROR_PATTERN" "$log"
}

last_log_error() {
    local log=$1
    [[ -f "$log" ]] || return 0
    grep -Ei -- "$ERROR_PATTERN" "$log" 2>/dev/null | tail -n 1 || true
}

config_default_model() {
    local config=$1
    awk '
        /^[[:space:]]*\[/ { in_table = 1 }
        !in_table && /^[[:space:]]*model[[:space:]]*=/ {
            value = $0
            sub(/^[^=]*=[[:space:]]*/, "", value)
            sub(/[[:space:]]*#.*/, "", value)
            gsub(/^[[:space:]"]+|[[:space:]"]+$/, "", value)
            print value
            exit
        }
    ' "$config"
}

config_available_models() {
    local config=$1
    awk '
        /^[[:space:]]*\[/ {
            in_nux = ($0 ~ /^[[:space:]]*\[tui\.model_availability_nux\][[:space:]]*$/)
            next
        }
        in_nux && /^[[:space:]]*[^#[:space:]][^=]*=/ {
            key = $0
            sub(/=.*/, "", key)
            gsub(/^[[:space:]"]+|[[:space:]"]+$/, "", key)
            print key
        }
    ' "$config"
}

load_models() {
    local config=$1 default_model=$2 model
    local -A seen=()
    AVAILABLE_MODELS=()
    # config는 TUI가 안내한 모델만 담아서 실제로 쓸 수 있는 id를 다 알지 못한다.
    # 알려진 id를 먼저 넣고, 새 id는 ZLINK_JOB_MODELS로 더한다(공백 구분).
    for model in ${ZLINK_JOB_MODELS:-gpt-6-astra gpt-5.6-sol gpt-5.6-terra gpt-5.6-luna}; do
        [[ -n "$model" && -z "${seen[$model]+present}" ]] || continue
        AVAILABLE_MODELS+=("$model")
        seen["$model"]=1
    done
    if [[ -n "$default_model" ]]; then
        AVAILABLE_MODELS+=("$default_model")
        seen["$default_model"]=1
    fi
    while IFS= read -r model; do
        [[ -n "$model" && -z "${seen[$model]+present}" ]] || continue
        AVAILABLE_MODELS+=("$model")
        seen["$model"]=1
    done < <(config_available_models "$config")
}

validate_model() {
    local wanted=$1 model
    for model in "${AVAILABLE_MODELS[@]}"; do
        [[ "$model" == "$wanted" ]] && return 0
    done
    printf '오류: 사용할 수 없는 모델 id입니다: %s\n' "$wanted" >&2
    printf '사용 가능한 모델:\n' >&2
    printf '  %s\n' "${AVAILABLE_MODELS[@]}" >&2
    [[ -n "$RESUME_COMMAND" ]] && printf '재개 명령: %s\n' "$RESUME_COMMAND" >&2
    exit 2
}

running_job_names() {
    local job_dir
    [[ -d "$ARTIFACT_ROOT" ]] || return 0
    for job_dir in "$ARTIFACT_ROOT"/*; do
        [[ -d "$job_dir" ]] || continue
        if job_is_running "$job_dir"; then
            printf '%s\n' "${job_dir##*/}"
        fi
    done
}

running_job_count() {
    local count=0 _name
    while IFS= read -r _name; do
        [[ -n "$_name" ]] && count=$((count + 1))
    done < <(running_job_names)
    printf '%s' "$count"
}

archive_file() {
    local file=$1 stamp target suffix=0
    [[ -f "$file" ]] || return 0
    stamp=$(date +%Y%m%d-%H%M%S)
    target="$file.$stamp"
    while [[ -e "$target" ]]; do
        suffix=$((suffix + 1))
        target="$file.$stamp.$suffix"
    done
    mv -- "$file" "$target"
}

write_meta() {
    local file=$1 model=$2 worktree=$3 effort=$4 started_at=$5 started_epoch=$6 ticks=$7
    {
        printf 'model=%s\n' "$model"
        printf 'worktree=%s\n' "$worktree"
        printf 'started_at=%s\n' "$started_at"
        printf 'started_at_epoch=%s\n' "$started_epoch"
        printf 'effort=%s\n' "$effort"
        printf 'proc_start_ticks=%s\n' "$ticks"
    } >"$file"
}

print_start_dry_run() {
    local model=$1 effort=$2 worktree=$3 brief=$4 log=$5
    printf '[dry-run] '
    quote_command codex exec -C "$worktree" -m "$model" -c "model_reasoning_effort=\"$effort\"" \
        "${SANDBOX_ARGS[@]}" \
        -o "${log%/job.log}/summary.md" -
    printf ' < '
    quote_command "$brief"
    printf ' > '
    quote_command "$log"
    printf ' 2>&1 &\n'
}

start_failure() {
    local job_dir=$1 message=$2
    cleanup_starting_process
    rm -f -- "$job_dir/.pid" "$job_dir/.meta" "$job_dir/.meta.tmp"
    printf '오류: %s\n' "$message" >&2
    printf '로그 마지막 20줄 (%s):\n' "$job_dir/job.log" >&2
    tail -n 20 "$job_dir/job.log" >&2 || true
    [[ -n "$RESUME_COMMAND" ]] && printf '재개 명령: %s\n' "$RESUME_COMMAND" >&2
    exit 1
}

start_command() {
    local name="" worktree="" brief="" model="" effort=high max_jobs="" config default_model
    local job_dir log top count started_at started_epoch ticks startup_seconds attempt brief_dir brief_abs
    while (($#)); do
        case "$1" in
            --worktree) [[ $# -ge 2 ]] || die 2 "--worktree에 경로가 필요합니다."; worktree=$2; shift 2 ;;
            --brief) [[ $# -ge 2 ]] || die 2 "--brief에 파일이 필요합니다."; brief=$2; shift 2 ;;
            --model) [[ $# -ge 2 ]] || die 2 "--model에 id가 필요합니다."; model=$2; shift 2 ;;
            --effort) [[ $# -ge 2 ]] || die 2 "--effort에 값이 필요합니다."; effort=$2; shift 2 ;;
            --max-jobs) [[ $# -ge 2 ]] || die 2 "--max-jobs에 값이 필요합니다."; max_jobs=$2; shift 2 ;;
            --read-only) SANDBOX_ARGS=(--sandbox read-only); shift ;;
            --dry-run) DRY_RUN=1; shift ;;
            --*) die 2 "알 수 없는 start 옵션입니다: $1" ;;
            *) [[ -z "$name" ]] || die 2 "job 이름은 하나만 지정할 수 있습니다."; name=$1; shift ;;
        esac
    done

    [[ -n "$name" && -n "$worktree" && -n "$brief" ]] \
        || die 2 "start에는 <이름>, --worktree <경로>, --brief <파일>이 필요합니다."
    validate_name "$name"
    [[ -f "$brief" && -s "$brief" ]] || die 2 "brief 파일이 없거나 비어 있습니다: $brief"
    if ! top=$(git -C "$worktree" rev-parse --show-toplevel 2>/dev/null); then
        die 2 "git worktree가 아닙니다: $worktree"
    fi
    worktree=$top
    case "$effort" in
        high|medium|low) ;;
        *) die 2 "지원하지 않는 effort입니다: $effort" ;;
    esac
    [[ -n "$max_jobs" ]] || max_jobs=${ZLINK_MAX_JOBS:-5}
    validate_positive_integer "동시 실행 상한" "$max_jobs"

    config="${HOME}/.codex/config.toml"
    [[ -f "$config" ]] || die 2 "Codex 설정 파일을 찾을 수 없습니다: $config"
    default_model=$(config_default_model "$config")
    [[ -n "$default_model" ]] || die 2 "Codex 설정에서 기본 model 값을 찾지 못했습니다: $config"
    load_models "$config" "$default_model"
    [[ -n "$model" ]] || model=$default_model
    validate_model "$model"
    require_command codex

    job_dir="$ARTIFACT_ROOT/$name"
    log="$job_dir/job.log"
    if [[ -d "$job_dir" ]] && job_is_running "$job_dir"; then
        die 2 "같은 이름의 job이 이미 실행 중입니다: $name"
    fi
    count=$(running_job_count)
    if ((count >= max_jobs)); then
        printf '현재 실행 중인 job:\n' >&2
        running_job_names | sed 's/^/  /' >&2
        die 2 "동시 실행 상한($max_jobs)에 도달했습니다."
    fi

    brief_dir=$(cd "$(dirname "$brief")" && pwd -P)
    brief_abs="$brief_dir/$(basename "$brief")"
    if ((DRY_RUN)); then
        print_start_dry_run "$model" "$effort" "$worktree" "$brief_abs" "$log"
        printf 'job 시작 예정: %s\n' "$name"
        printf '로그: %s\n' "$log"
        return 0
    fi

    mkdir -p -- "$job_dir"
    rm -f -- "$job_dir/.pid" "$job_dir/.meta.tmp"
    archive_file "$log"
    archive_file "$job_dir/summary.md"
    if [[ "$brief_abs" != "$job_dir/brief.md" ]]; then
        cp -- "$brief_abs" "$job_dir/brief.md"
    fi

    started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    started_epoch=$(date +%s)
    (
        cd "$worktree"
        export ZLINK_JOB_DIR="$job_dir"
        exec codex exec -C "$worktree" -m "$model" -c "model_reasoning_effort=\"$effort\"" \
            "${SANDBOX_ARGS[@]}" \
            -o "$job_dir/summary.md" - \
            <"$job_dir/brief.md"
    ) >"$log" 2>&1 &
    STARTING_PID=$!
    STARTING_TICKS=$(proc_start_ticks "$STARTING_PID")

    startup_seconds=${ZLINK_JOB_STARTUP_SECONDS:-20}
    validate_positive_integer "시작 확인 시간" "$startup_seconds"
    ((startup_seconds <= 20)) || die 2 "시작 확인 시간은 20초를 넘을 수 없습니다: $startup_seconds"
    for ((attempt = 0; attempt < startup_seconds * 2; attempt++)); do
        [[ -n "$STARTING_TICKS" ]] || STARTING_TICKS=$(proc_start_ticks "$STARTING_PID")
        if log_has_error "$log"; then
            start_failure "$job_dir" "job 시작 로그에서 오류를 감지했습니다: $name"
        fi
        if ! pid_matches "$STARTING_PID" "$STARTING_TICKS"; then
            start_failure "$job_dir" "job이 시작 직후 종료되었습니다: $name"
        fi
        sleep 0.5
    done
    if log_has_error "$log"; then
        start_failure "$job_dir" "job 시작 로그에서 오류를 감지했습니다: $name"
    fi
    if ! pid_matches "$STARTING_PID" "$STARTING_TICKS"; then
        start_failure "$job_dir" "job이 시작 직후 종료되었습니다: $name"
    fi

    write_meta "$job_dir/.meta.tmp" "$model" "$worktree" "$effort" \
        "$started_at" "$started_epoch" "$STARTING_TICKS"
    mv -- "$job_dir/.meta.tmp" "$job_dir/.meta"
    printf '%s\n' "$STARTING_PID" >"$job_dir/.pid"
    printf 'job 시작: %s (pid %s)\n' "$name" "$STARTING_PID"
    printf '로그: %s\n' "$log"
    STARTING_PID=""
    STARTING_TICKS=""
}

job_state() {
    local job_dir=$1
    # 보고서를 남겼으면 도중에 오류 줄이 있었어도 job은 끝난 것이다.
    # 오류는 보고서가 없을 때만 실패와 죽음을 가른다.
    if job_is_running "$job_dir"; then
        printf '실행'
    elif [[ -f "$job_dir/summary.md" ]]; then
        printf '완료'
    elif log_has_error "$job_dir/job.log"; then
        printf '실패'
    else
        printf '죽음'
    fi
}

elapsed_time() {
    local started=$1 now elapsed days hours minutes seconds
    [[ "$started" =~ ^[0-9]+$ ]] || { printf '-'; return 0; }
    now=$(date +%s)
    ((now >= started)) || { printf '-'; return 0; }
    elapsed=$((now - started))
    days=$((elapsed / 86400))
    hours=$(((elapsed % 86400) / 3600))
    minutes=$(((elapsed % 3600) / 60))
    seconds=$((elapsed % 60))
    if ((days > 0)); then
        printf '%s일 %s시간' "$days" "$hours"
    elif ((hours > 0)); then
        printf '%s시간 %s분' "$hours" "$minutes"
    elif ((minutes > 0)); then
        printf '%s분 %s초' "$minutes" "$seconds"
    else
        printf '%s초' "$seconds"
    fi
}

print_status_row() {
    local job_dir=$1 name model worktree started state summary error elapsed
    name=${job_dir##*/}
    model=$(meta_value "$job_dir/.meta" model)
    worktree=$(meta_value "$job_dir/.meta" worktree)
    started=$(meta_value "$job_dir/.meta" started_at_epoch)
    state=$(job_state "$job_dir")
    [[ -f "$job_dir/summary.md" ]] && summary='있음' || summary='없음'
    # 완료했거나 아직 도는 job의 중간 오류는 결과가 아니므로 표에 싣지 않는다.
    if [[ "$state" == '완료' ]]; then
        error=''
    else
        error=$(last_log_error "$job_dir/job.log")
    fi
    error=${error//$'\t'/ }
    error=${error//$'\r'/ }
    error=${error//$'\n'/ }
    [[ -n "$error" ]] || error='-'
    ((${#error} <= 160)) || error="${error:0:157}..."
    elapsed=$(elapsed_time "$started")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$name" "${model:--}" "${worktree:--}" \
        "$elapsed" "$state" "$summary" "$error"
}


# 최근 창(기본 24시간) 안에 job.log가 갱신됐는가. find 구현마다 상대 시각 표기가 달라 stat으로 비교한다.
recently_active() {
    local log="$1/job.log" window=${ZLINK_JOB_RECENT_SECONDS:-7200} mtime now
    [[ -f "$log" ]] || return 1
    mtime=$(stat -c %Y "$log" 2>/dev/null) || return 1
    now=$(date +%s)
    (( now - mtime <= window ))
}

status_command() {
    local name="" job_dir found=0
    STATUS_ALL=0
    while (($#)); do
        case "$1" in
            --all) STATUS_ALL=1; shift ;;
            --dry-run) DRY_RUN=1; shift ;;
            --*) die 2 "알 수 없는 status 옵션입니다: $1" ;;
            *) [[ -z "$name" ]] || die 2 "status 이름은 하나만 지정할 수 있습니다."; name=$1; shift ;;
        esac
    done
    printf '이름\t모델\tworktree\t경과\t상태\tsummary\t로그 마지막 오류\n'
    if [[ -n "$name" ]]; then
        validate_name "$name"
        job_dir="$ARTIFACT_ROOT/$name"
        [[ -d "$job_dir" ]] || die 1 "job을 찾을 수 없습니다: $name"
        print_status_row "$job_dir"
        return 0
    fi
    [[ -d "$ARTIFACT_ROOT" ]] || return 0
    for job_dir in "$ARTIFACT_ROOT"/*; do
        [[ -d "$job_dir" ]] || continue
        if ((STATUS_ALL == 0)); then
            # 기본은 지금 도는 job과 최근 2시간 안에 움직인 job만 본다(ZLINK_JOB_RECENT_SECONDS로 조정, --all은 전부).
            [[ -f "$job_dir/.pid" ]] || recently_active "$job_dir" || continue
        fi
        print_status_row "$job_dir"
        found=1
    done
    ((found == 0)) || return 0
}

watch_command() {
    local interval=180 job_dir name state timestamp first_scan=1
    local -A seen=()
    while (($#)); do
        case "$1" in
            --interval) [[ $# -ge 2 ]] || die 2 "--interval에 초가 필요합니다."; interval=$2; shift 2 ;;
            --dry-run) DRY_RUN=1; shift ;;
            *) die 2 "알 수 없는 watch 인자입니다: $1" ;;
        esac
    done
    validate_positive_integer "감시 간격" "$interval"
    while true; do
        if [[ -d "$ARTIFACT_ROOT" ]]; then
            for job_dir in "$ARTIFACT_ROOT"/*; do
                [[ -d "$job_dir" ]] || continue
                name=${job_dir##*/}
                state=$(job_state "$job_dir")
                if [[ -z "${seen[$name]+present}" ]]; then
                    seen["$name"]=$state
                    if ((first_scan == 0)); then
                        timestamp=$(date '+%Y-%m-%d %H:%M:%S')
                        printf '%s\t%s\t없음 → %s\n' "$timestamp" "$name" "$state"
                    fi
                elif [[ "${seen[$name]}" != "$state" ]]; then
                    timestamp=$(date '+%Y-%m-%d %H:%M:%S')
                    printf '%s\t%s\t%s → %s\n' "$timestamp" "$name" "${seen[$name]}" "$state"
                    seen["$name"]=$state
                fi
            done
        fi
        first_scan=0
        sleep "$interval"
    done
}

kill_command() {
    local name="" job_dir pid expected_ticks
    while (($#)); do
        case "$1" in
            --dry-run) DRY_RUN=1; shift ;;
            --*) die 2 "알 수 없는 kill 옵션입니다: $1" ;;
            *) [[ -z "$name" ]] || die 2 "kill 이름은 하나만 지정할 수 있습니다."; name=$1; shift ;;
        esac
    done
    [[ -n "$name" ]] || die 2 "kill에는 <이름>이 필요합니다."
    validate_name "$name"
    job_dir="$ARTIFACT_ROOT/$name"
    [[ -d "$job_dir" ]] || die 1 "job을 찾을 수 없습니다: $name"
    pid=$(job_pid "$job_dir") || die 1 "기록된 pid가 없습니다: $name"
    expected_ticks=$(meta_value "$job_dir/.meta" proc_start_ticks)
    if ! pid_matches "$pid" "$expected_ticks"; then
        die 1 "기록된 pid가 실행 중이 아니거나 다른 프로세스가 재사용했습니다: $pid"
    fi
    if ((DRY_RUN)); then
        printf '[dry-run] '
        quote_command kill -TERM "$pid"
        printf '\n'
        return 0
    fi
    stop_pid "$pid" "$expected_ticks"
    rm -f -- "$job_dir/.pid"
    printf 'job 종료: %s (pid %s)\n' "$name" "$pid"
}

logs_command() {
    local name="" lines=20 job_dir error
    while (($#)); do
        case "$1" in
            -n) [[ $# -ge 2 ]] || die 2 "-n에 줄 수가 필요합니다."; lines=$2; shift 2 ;;
            --dry-run) DRY_RUN=1; shift ;;
            --*) die 2 "알 수 없는 logs 옵션입니다: $1" ;;
            *) [[ -z "$name" ]] || die 2 "logs 이름은 하나만 지정할 수 있습니다."; name=$1; shift ;;
        esac
    done
    [[ -n "$name" ]] || die 2 "logs에는 <이름>이 필요합니다."
    validate_name "$name"
    validate_positive_integer "로그 줄 수" "$lines"
    job_dir="$ARTIFACT_ROOT/$name"
    [[ -f "$job_dir/job.log" ]] || die 1 "job 로그를 찾을 수 없습니다: $name"
    printf '로그 마지막 %s줄 (%s):\n' "$lines" "$job_dir/job.log"
    tail -n "$lines" "$job_dir/job.log"
    error=$(last_log_error "$job_dir/job.log")
    if [[ -n "$error" ]]; then
        printf '감지된 오류: %s\n' "$error"
    else
        printf '감지된 오류: 없음\n'
    fi
}

main() {
    local original=("$0" "$@") args=() command arg
    local -a AVAILABLE_MODELS=()
    require_command git
    for arg in "$@"; do
        if [[ "$arg" == --dry-run ]]; then
            DRY_RUN=1
        else
            args+=("$arg")
        fi
    done
    ((${#args[@]} > 0)) || { usage; exit 2; }
    command=${args[0]}
    RESUME_COMMAND=$(quote_command "${original[@]}")
    # job 디렉터리는 기본 worktree 하나에 모은다 — 작업 worktree에서 실행해도 같은 목록을 본다.
    ARTIFACT_ROOT=${ZLINK_JOB_ROOT:-$(primary_worktree)/.artifacts/codex}
    set -- "${args[@]:1}"
    case "$command" in
        start) start_command "$@" ;;
        status) status_command "$@" ;;
        watch) watch_command "$@" ;;
        kill) kill_command "$@" ;;
        logs) logs_command "$@" ;;
        -h|--help|help) usage ;;
        *) usage; die 2 "알 수 없는 명령입니다: $command" ;;
    esac
}

main "$@"
