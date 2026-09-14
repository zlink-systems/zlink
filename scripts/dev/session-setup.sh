#!/usr/bin/env bash

set -euo pipefail

# 개발 세션의 공통 전제만 점검·보정한다. 큐와 패키지는 소유 스크립트가
# 따로 있으므로 여기서는 시작하거나 생성하지 않는다.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$SCRIPT_DIR" rev-parse --show-toplevel)"

DRY_RUN=0
CHECK_ONLY=0
RESUME_COMMAND=""

RESERVED_PORTS="5200-5299,6200-6219"
TMP_DIR="${ZLINK_TMP_DIR:-/tmp}"
TMPFS_MIN_FREE_KB="${ZLINK_TMPFS_MIN_FREE_KB:-1048576}"
JOB_TMP_MAX_AGE_DAYS="${ZLINK_JOB_TMP_MAX_AGE_DAYS:-7}"
# 측정 큐는 저장소 전체에 하나뿐이므로 기본 worktree 기준으로 찾는다.
PRIMARY_WORKTREE="$(git worktree list --porcelain 2>/dev/null \
    | awk '$1 == "worktree" { print substr($0, index($0, " ") + 1); exit }')"
[ -n "$PRIMARY_WORKTREE" ] || PRIMARY_WORKTREE="$REPO_ROOT"
PERF_QUEUE_ROOT="${ZLINK_PERF_QUEUE:-$PRIMARY_WORKTREE/.artifacts/perf-queue}"
LOCAL_PACKAGE_ROOT="${ZLINK_LOCAL_PACKAGE_ROOT:-$REPO_ROOT/.artifacts/wsl}"
SYSCTL_BIN="${ZLINK_SYSCTL_BIN:-sysctl}"
DF_BIN="${ZLINK_DF_BIN:-df}"
UNAME_BIN="${ZLINK_UNAME_BIN:-uname}"

PORT_STATUS=""
TMPFS_STATUS=""
QUEUE_STATUS=""
CORE_STATUS=""
PACKAGE_STATUS=""
declare -a OLD_JOB_TMP_DIRS=()

usage() {
    cat <<'EOF'
사용법: session-setup.sh [--check] [--dry-run]

세션 시작 전 벤치 포트 예약, /tmp 여유, 성능 큐 runner, Core release prefix와
로컬 binding package 상태를 확인합니다. 기본 실행은 빠진 포트 예약만 설정합니다.

환경 변수:
  ZLINK_TMPFS_MIN_FREE_KB     /tmp 최소 여유 KiB (기본 1048576)
  ZLINK_JOB_TMP_MAX_AGE_DAYS  표시할 zlink-job-* 후보의 최소 경과 일수 (기본 7)
  ZLINK_PERF_QUEUE            성능 티켓 큐 루트
  ZLINK_LOCAL_PACKAGE_ROOT    로컬 binding package 출력 루트
  ZLINK_CORE_CACHE_DIR         Core release cache 루트
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

run_mutation() {
    if ((DRY_RUN || CHECK_ONLY)); then
        print_command "$@"
        return 0
    fi
    "$@"
}

state_summary() {
    [[ -n "$RESUME_COMMAND" ]] && printf '재개 명령: %s\n' "$RESUME_COMMAND" >&2
    return 0
}

on_error() {
    local code=$1 line=$2
    trap - ERR
    printf '오류: %s행에서 명령이 실패했습니다 (exit %s).\n' "$line" "$code" >&2
    state_summary
    exit "$code"
}
trap 'on_error $? $LINENO' ERR

die() {
    local code=$1
    shift
    printf '오류: %s\n' "$*" >&2
    state_summary
    exit "$code"
}

require_nonnegative_integer() {
    local value=$1 name=$2
    [[ "$value" =~ ^[0-9]+$ ]] || die 2 "$name 값은 0 이상의 정수여야 합니다: $value"
}

is_reserved() {
    local current=$1 wanted_start=$2 wanted_end=$3 item start end
    local entries=()
    IFS=',' read -r -a entries <<<"$current"
    for item in "${entries[@]}"; do
        [[ "$item" =~ ^[0-9]+(-[0-9]+)?$ ]] || continue
        start=${item%-*}
        end=${item#*-}
        [[ "$item" == *-* ]] || end=$start
        if ((start <= wanted_start && end >= wanted_end)); then
            return 0
        fi
    done
    return 1
}

reserve_benchmark_ports() {
    local current
    if ! current=$("$SYSCTL_BIN" -n net.ipv4.ip_local_reserved_ports 2>/dev/null); then
        PORT_STATUS="확인 실패 (sysctl 읽기 불가)"
        return 0
    fi

    if is_reserved "$current" 5200 5299 && is_reserved "$current" 6200 6219; then
        PORT_STATUS="예약됨 ($current)"
        return 0
    fi

    if ((CHECK_ONLY)); then
        PORT_STATUS="미예약 (--check: 변경하지 않음)"
        return 0
    fi
    if ((DRY_RUN)); then
        run_mutation "$SYSCTL_BIN" -w "net.ipv4.ip_local_reserved_ports=$RESERVED_PORTS"
        PORT_STATUS="미예약 (dry-run: 설정 예정)"
        return 0
    fi

    run_mutation "$SYSCTL_BIN" -w "net.ipv4.ip_local_reserved_ports=$RESERVED_PORTS" >/dev/null
    PORT_STATUS="예약 설정됨 ($RESERVED_PORTS)"
}

collect_old_job_tmp_dirs() {
    local path
    [[ -d "$TMP_DIR" ]] || return 0
    while IFS= read -r path; do
        OLD_JOB_TMP_DIRS+=("$path")
    done < <(find "$TMP_DIR" -mindepth 1 -maxdepth 1 -type d -name 'zlink-job-*' \
        -mtime "+$JOB_TMP_MAX_AGE_DAYS" -printf '%T@\t%p\n' 2>/dev/null \
        | sort -n | cut -f2-)
}

check_tmpfs_space() {
    local available_kb use_percent
    require_nonnegative_integer "$TMPFS_MIN_FREE_KB" "ZLINK_TMPFS_MIN_FREE_KB"
    require_nonnegative_integer "$JOB_TMP_MAX_AGE_DAYS" "ZLINK_JOB_TMP_MAX_AGE_DAYS"
    if [[ ! -d "$TMP_DIR" ]]; then
        TMPFS_STATUS="확인 실패 (디렉터리 없음: $TMP_DIR)"
        return 0
    fi
    if ! read -r available_kb use_percent < <("$DF_BIN" -Pk "$TMP_DIR" 2>/dev/null \
        | awk 'NR == 2 { print $4, $5 }'); then
        TMPFS_STATUS="확인 실패 (df 읽기 불가)"
        return 0
    fi
    if [[ ! "$available_kb" =~ ^[0-9]+$ ]]; then
        TMPFS_STATUS="확인 실패 (df 형식)"
        return 0
    fi
    if ((available_kb < TMPFS_MIN_FREE_KB)); then
        collect_old_job_tmp_dirs
        TMPFS_STATUS="여유 부족 (${available_kb}KiB < ${TMPFS_MIN_FREE_KB}KiB, 사용 ${use_percent:-?})"
    else
        TMPFS_STATUS="여유 충분 (${available_kb}KiB, 사용 ${use_percent:-?})"
    fi
}

check_perf_queue_runner() {
    local pid_file="$PERF_QUEUE_ROOT/runner.pid" pid
    if [[ ! -r "$pid_file" ]]; then
        QUEUE_STATUS="runner 없음 (시작하지 않음)"
        return 0
    fi
    IFS= read -r pid <"$pid_file" || true
    if [[ ! "$pid" =~ ^[1-9][0-9]*$ ]]; then
        QUEUE_STATUS="runner.pid 형식 오류 (시작하지 않음)"
    elif kill -0 "$pid" 2>/dev/null; then
        QUEUE_STATUS="실행 중 (pid $pid)"
    else
        QUEUE_STATUS="runner 없음 (stale pid $pid, 시작하지 않음)"
    fi
}

core_platform() {
    local system machine
    system=$("$UNAME_BIN" -s)
    machine=$("$UNAME_BIN" -m)
    case "$system/$machine" in
        Linux/x86_64|Linux/amd64) printf 'linux-x64\n' ;;
        Linux/aarch64|Linux/arm64) printf 'linux-arm64\n' ;;
        Darwin/x86_64|Darwin/amd64) printf 'macos-x64\n' ;;
        Darwin/arm64|Darwin/aarch64) printf 'macos-arm64\n' ;;
        MINGW*/x86_64|MSYS*/x86_64|CYGWIN*/x86_64) printf 'windows-x64\n' ;;
        *) return 1 ;;
    esac
}

check_core_prefix() {
    local version platform cache_root prefix manifest manifest_version
    version="${ZLINK_CORE_RELEASE_VERSION:-$(sed -n 's/^LIBZLINK_VERSION=//p' "$REPO_ROOT/VERSION")}"
    platform="${ZLINK_CORE_RELEASE_PLATFORM:-$(core_platform || true)}"
    cache_root="${ZLINK_CORE_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/zlink/core}"
    if [[ -z "$version" || -z "$platform" ]]; then
        CORE_STATUS="확인 실패 (버전 또는 플랫폼을 알 수 없음)"
        return 0
    fi
    prefix="$cache_root/$version/$platform"
    manifest="$prefix/share/zlink/core-package-provenance.json"
    if [[ ! -f "$manifest" ]]; then
        CORE_STATUS="없음 ($prefix)"
        return 0
    fi
    manifest_version=$(sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -n1)
    if [[ "$manifest_version" == "$version" ]]; then
        CORE_STATUS="준비됨 ($prefix)"
    else
        CORE_STATUS="version 불일치 ($prefix)"
    fi
}

has_package() {
    local pattern=$1
    compgen -G "$LOCAL_PACKAGE_ROOT/$pattern" >/dev/null
}

check_local_packages() {
    local core_version language binding_version count=0
    core_version=$(sed -n 's/^LIBZLINK_VERSION=//p' "$REPO_ROOT/VERSION")
    if [[ -z "$core_version" || ! -d "$LOCAL_PACKAGE_ROOT" ]]; then
        PACKAGE_STATUS="없음 ($LOCAL_PACKAGE_ROOT)"
        return 0
    fi
    has_package "c/zlink-c-$core_version.tar.gz" && ((count += 1))
    for language in cpp dotnet go java node python rust; do
        binding_version=$(sed -n 's/^ZLINK_BINDING_VERSION=//p' "$REPO_ROOT/bindings/$language/VERSION")
        [[ -n "$binding_version" ]] || continue
        case "$language" in
            cpp) has_package "install/zlink-cpp/$binding_version/include/zlink.hpp" && ((count += 1)) ;;
            dotnet) has_package "nuget/Zlink.$binding_version.nupkg" && ((count += 1)) ;;
            go) has_package "go/zlink-go-$binding_version.tar.gz" && ((count += 1)) ;;
            java) has_package "maven/systems/zlink/zlink/$binding_version/zlink-$binding_version.jar" && ((count += 1)) ;;
            node) has_package "npm/zlink-systems-zlink-$binding_version.tgz" && ((count += 1)) ;;
            python) has_package "python/zlink-$binding_version-*.whl" && ((count += 1)) ;;
            rust) has_package "rust/zlink-$binding_version.crate" && ((count += 1)) ;;
        esac
    done
    if ((count == 0)); then
        PACKAGE_STATUS="없음 ($LOCAL_PACKAGE_ROOT)"
    else
        PACKAGE_STATUS="존재 (${count}/8 언어, $LOCAL_PACKAGE_ROOT)"
    fi
}

print_summary() {
    printf '\n세션 준비 상태\n'
    printf '%-14s | %s\n' '항목' '상태'
    printf '%-14s-+-%s\n' '---------------' '----------------------------------------'
    printf '%-14s | %s\n' '벤치 포트' "$PORT_STATUS"
    printf '%-14s | %s\n' '/tmp tmpfs' "$TMPFS_STATUS"
    printf '%-14s | %s\n' '성능 큐 runner' "$QUEUE_STATUS"
    printf '%-14s | %s\n' 'Core release' "$CORE_STATUS"
    printf '%-14s | %s\n' '로컬 패키지' "$PACKAGE_STATUS"
    if ((${#OLD_JOB_TMP_DIRS[@]})); then
        printf '\n삭제 후보: %s일 이상 지난 zlink-job-* 임시 디렉터리 (자동 삭제하지 않음)\n' "$JOB_TMP_MAX_AGE_DAYS"
        printf '  %s\n' "${OLD_JOB_TMP_DIRS[@]}"
    fi
}

main() {
    while (($#)); do
        case "$1" in
            --check) CHECK_ONLY=1; shift ;;
            --dry-run) DRY_RUN=1; shift ;;
            -h|--help) usage; return 0 ;;
            *) usage >&2; die 2 "알 수 없는 인자: $1" ;;
        esac
    done
    RESUME_COMMAND="$(quote_command "$0" --check)"
    reserve_benchmark_ports
    check_tmpfs_space
    check_perf_queue_runner
    check_core_prefix
    check_local_packages
    print_summary
}

main "$@"
