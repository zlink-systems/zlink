#!/usr/bin/env bash

set -euo pipefail

# GitHub Project "ZLink". 환경변수로 다른 Project에서 검증할 수 있다.
ZLINK_PROJECT_ID="${ZLINK_PROJECT_ID:-PVT_kwHOErOLTs4Bi-bl}"
ZLINK_PROJECT_OWNER="${ZLINK_PROJECT_OWNER:-zlink-systems}"
ZLINK_PROJECT_NUMBER="${ZLINK_PROJECT_NUMBER:-1}"
ZLINK_PROJECT_STATUS_FIELD="${ZLINK_PROJECT_STATUS_FIELD:-Status}"
ZLINK_PROJECT_STATUS_FIELD_ID="${ZLINK_PROJECT_STATUS_FIELD_ID:-PVTSSF_lAHOErOLTs4Bi-blzhh0gcI}"
ZLINK_BASELINE_ROOT="${ZLINK_BASELINE_ROOT:-${HOME}/project/zlink}"

DRY_RUN=0
ISSUE_NUMBER=""
ISSUE_URL=""
ISSUE_TITLE=""
WORK_BRANCH=""
WORKTREE_PATH=""
RESUME_COMMAND=""

usage() {
    cat <<'EOF'
사용법:
  work.sh [--dry-run] start "<제목>" --area <area> --kind <kind> --body <파일> [--milestone <이름>] [--no-packages]
  work.sh [--dry-run] start --issue <N> [--no-packages]
  work.sh [--dry-run] pr --body <파일> [--closes|--refs]
  work.sh [--dry-run] status [--milestone <이름>]
  work.sh [--dry-run] done --verified <sha>
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
    if ((DRY_RUN)); then
        print_command "$@"
        return 0
    fi
    "$@"
}

run_mutation_capture() {
    if ((DRY_RUN)); then
        print_command "$@" >&2
        return 0
    fi
    "$@"
}

warn() {
    printf '경고: %s\n' "$*" >&2
}

state_summary() {
    [[ -n "$ISSUE_NUMBER" ]] && printf '  Issue: #%s%s\n' "$ISSUE_NUMBER" "${ISSUE_URL:+ ($ISSUE_URL)}" >&2
    [[ -n "$WORK_BRANCH" ]] && printf '  브랜치: %s\n' "$WORK_BRANCH" >&2
    [[ -n "$WORKTREE_PATH" ]] && printf '  worktree: %s\n' "$WORKTREE_PATH" >&2
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

require_file() {
    [[ -f "$1" ]] || die 2 "파일이 없거나 일반 파일이 아닙니다: $1"
}

repo_root() {
    git rev-parse --show-toplevel 2>/dev/null || die 1 "Git 저장소 안에서 실행해야 합니다."
}

current_branch() {
    git branch --show-current
}

validate_area() {
    case "$1" in
        core|bindings|framework-dotnet|framework-java|framework-node|framework-cpp|bench|ci|docs) ;;
        *) die 2 "지원하지 않는 area입니다: $1" ;;
    esac
}

validate_kind() {
    case "$1" in
        bug|perf|feature|chore) ;;
        *) die 2 "지원하지 않는 kind입니다: $1" ;;
    esac
}

validate_issue_body() {
    local body=$1
    require_file "$body"
    if ! awk '
        function section(line, clean) {
            clean = line
            sub(/^[[:space:]]*#{1,6}[[:space:]]*/, "", clean)
            sub(/^[[:space:]]*[-*][[:space:]]*/, "", clean)
            if (clean ~ /^범위[[:space:]]*:/ || clean == "범위") return 1
            if (clean ~ /^완료[[:space:]]*조건[[:space:]]*:/ || clean ~ /^완료[[:space:]]*조건$/) return 2
            if (clean ~ /^근거[[:space:]]*:/ || clean == "근거") return 3
            return 0
        }
        {
            s = section($0)
            if (s) {
                current = s
                value = $0
                sub(/^.*범위[[:space:]]*:[[:space:]]*/, "", value)
                sub(/^.*완료[[:space:]]*조건[[:space:]]*:[[:space:]]*/, "", value)
                sub(/^.*근거[[:space:]]*:[[:space:]]*/, "", value)
                if (value != $0 && value ~ /[^[:space:]]/) found[s] = 1
                next
            }
            if ($0 ~ /^[[:space:]]*#{1,6}[[:space:]]+/) current = 0
            if (current && $0 !~ /^[[:space:]]*$/ && $0 !~ /^[[:space:]]*<!--/) found[current] = 1
        }
        END { exit !(found[1] && found[2] && found[3]) }
    ' "$body"; then
        die 2 "Issue 본문에는 비어 있지 않은 '범위', '완료 조건', '근거' 항목이 모두 필요합니다."
    fi
}

slugify() {
    local slug
    slug=$(printf '%s' "$1" \
        | tr '[:upper:]' '[:lower:]' \
        | sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//' \
        | cut -c1-40 \
        | sed -E 's/-+$//')
    [[ -n "$slug" ]] || slug=work
    printf '%s\n' "$slug"
}

default_milestone() {
    local root=$1 conf value
    conf="$root/scripts/dev/work.conf"
    [[ -f "$conf" ]] || return 0
    value=$(sed -n -E '/^[[:space:]]*current_milestone[[:space:]]*=/ {
        s/^[^=]*=//; s/[[:space:]]*#.*$//; s/^[[:space:]\"]+//; s/[[:space:]\"]+$//; p; q
    }' "$conf")
    value=${value%"${value##*[![:space:]]}"}
    printf '%s' "$value"
}

find_issue_by_title() {
    local wanted=$1 number title url
    while IFS=$'\t' read -r number title url; do
        if [[ "$title" == "$wanted" ]]; then
            printf '%s\t%s\n' "$number" "$url"
            return 0
        fi
    done < <(gh issue list --state all --limit 100 --search "$wanted in:title" \
        --json number,title,url --jq '.[] | [.number,.title,.url] | @tsv')
}

load_issue() {
    local number=$1
    ISSUE_NUMBER=$number
    ISSUE_TITLE=$(gh issue view "$number" --json title --jq '.title')
    ISSUE_URL=$(gh issue view "$number" --json url --jq '.url')
    [[ -n "$ISSUE_TITLE" && -n "$ISSUE_URL" ]] || die 1 "Issue #$number 정보를 읽지 못했습니다."
}

issue_label_value() {
    local number=$1 prefix=$2 label value
    while IFS= read -r label; do
        if [[ "$label" == "$prefix"* ]]; then
            value=${label#"$prefix"}
            value=${value#"${value%%[![:space:]]*}"}
            value=${value%"${value##*[![:space:]]}"}
            printf '%s\n' "$value"
            return 0
        fi
    done < <(gh issue view "$number" --json labels --jq '.labels[].name')
}

set_milestone_best_effort() {
    local number=$1 milestone=$2 current
    [[ -n "$milestone" ]] || return 0
    if ! current=$(gh issue view "$number" --json milestone --jq '.milestone.title // empty' 2>/dev/null); then
        warn "Issue milestone 상태를 확인하지 못했습니다. 로컬 작업은 계속합니다."
        return 0
    fi
    [[ "$current" == "$milestone" ]] && return 0
    if ((DRY_RUN)); then
        print_command gh issue edit "$number" --milestone "$milestone"
    elif ! gh issue edit "$number" --milestone "$milestone" >/dev/null; then
        warn "Issue #$number milestone '$milestone' 설정에 실패했습니다. 로컬 작업은 계속합니다."
    fi
}

project_status_best_effort() {
    local issue_url=$1 status_name=$2 rows item_id option_id added
    [[ -n "$issue_url" ]] || { warn "Project 상태 갱신을 위한 Issue URL이 없습니다."; return 0; }

    if ! rows=$(gh project item-list "$ZLINK_PROJECT_NUMBER" --owner "$ZLINK_PROJECT_OWNER" \
        --limit 1000 --format json --jq '.items[] | [.id,.content.url] | @tsv' 2>/dev/null); then
        warn "Project 항목 조회에 실패했습니다. '$status_name' 갱신을 건너뜁니다."
        return 0
    fi
    item_id=$(awk -F '\t' -v url="$issue_url" '$2 == url { print $1; exit }' <<<"$rows")

    if [[ -z "$item_id" ]]; then
        if ((DRY_RUN)); then
            print_command gh project item-add "$ZLINK_PROJECT_NUMBER" --owner "$ZLINK_PROJECT_OWNER" --url "$issue_url"
            item_id='<새 항목 ID>'
        else
            if ! added=$(gh project item-add "$ZLINK_PROJECT_NUMBER" --owner "$ZLINK_PROJECT_OWNER" \
                --url "$issue_url" --format json --jq '.id' 2>/dev/null); then
                warn "Issue를 Project에 추가하지 못했습니다. '$status_name' 갱신을 건너뜁니다."
                return 0
            fi
            item_id=$added
        fi
    fi

    if ! option_id=$(gh project field-list "$ZLINK_PROJECT_NUMBER" --owner "$ZLINK_PROJECT_OWNER" \
        --format json --jq ".fields[] | select(.id == \"$ZLINK_PROJECT_STATUS_FIELD_ID\" or .name == \"$ZLINK_PROJECT_STATUS_FIELD\") | .options[] | select(.name == \"$status_name\") | .id" 2>/dev/null); then
        warn "Project Status 옵션 '$status_name' 조회에 실패했습니다."
        return 0
    fi
    if [[ -z "$option_id" ]]; then
        warn "Project Status 옵션 '$status_name'을 찾지 못했습니다."
        return 0
    fi

    if ((DRY_RUN)); then
        print_command gh project item-edit --project-id "$ZLINK_PROJECT_ID" --id "$item_id" \
            --field-id "$ZLINK_PROJECT_STATUS_FIELD_ID" --single-select-option-id "$option_id"
    elif ! gh project item-edit --project-id "$ZLINK_PROJECT_ID" --id "$item_id" \
        --field-id "$ZLINK_PROJECT_STATUS_FIELD_ID" --single-select-option-id "$option_id" >/dev/null; then
        warn "Project 상태를 '$status_name'(으)로 바꾸지 못했습니다. 로컬 작업은 계속합니다."
    fi
}

worktree_for_branch() {
    local branch=$1
    git worktree list --porcelain | awk -v ref="refs/heads/$branch" '
        $1 == "worktree" { path = substr($0, index($0, " ") + 1) }
        $1 == "branch" && $2 == ref { print path; exit }
    '
}

existing_branch_for_issue() {
    local number=$1 branch
    branch=$(current_branch)
    if [[ "$branch" =~ ^[^/]+/$number-[a-z0-9-]+$ ]]; then
        printf '%s\n' "$branch"
        return 0
    fi
    branch=$(git worktree list --porcelain | awk -v pattern="/$number-[a-z0-9-]+$" '
        $1 == "branch" {
            value = $2
            sub(/^refs\/heads\//, "", value)
            if (value ~ pattern) { print value; exit }
        }
    ')
    if [[ -n "$branch" ]]; then
        printf '%s\n' "$branch"
        return 0
    fi
    git for-each-ref --format='%(refname:short)' refs/heads refs/remotes/origin \
        | sed 's#^origin/##' \
        | awk -v pattern="/$number-[a-z0-9-]+$" '$0 ~ pattern { print; exit }'
}

prepare_packages() {
    local worktree=$1
    run_mutation bash "$worktree/scripts/local-package/build-wsl.sh"
    printf '패키지 준비: %s/.artifacts/wsl\n' "$worktree"
}

ensure_worktree() {
    local root=$1 branch=$2 target=$3 existing remote_exists=0
    existing=$(worktree_for_branch "$branch")
    if [[ -n "$existing" ]]; then
        WORKTREE_PATH=$existing
        printf 'worktree 재사용: %s\n' "$existing"
        return 0
    fi
    [[ ! -e "$target" ]] || die 1 "등록되지 않은 경로가 이미 존재합니다: $target"

    if git show-ref --verify --quiet "refs/heads/$branch"; then
        run_mutation git worktree add "$target" "$branch"
    else
        run_mutation git fetch origin
        git show-ref --verify --quiet "refs/remotes/origin/$branch" && remote_exists=1
        if ((remote_exists)); then
            run_mutation git worktree add -b "$branch" "$target" "origin/$branch"
        else
            run_mutation git worktree add "$target" -b "$branch" origin/main
        fi
    fi
    WORKTREE_PATH=$target
    printf 'worktree 준비: %s\n' "$target"
}

start_command() {
    local root title="" area="" kind="" body="" milestone="" issue_arg="" no_packages=0
    local found found_number _found_url existing_kind slug target
    root=$(repo_root)

    while (($#)); do
        case "$1" in
            --issue) [[ $# -ge 2 ]] || die 2 "--issue에 번호가 필요합니다."; issue_arg=$2; shift 2 ;;
            --area) [[ $# -ge 2 ]] || die 2 "--area에 값이 필요합니다."; area=$2; shift 2 ;;
            --kind) [[ $# -ge 2 ]] || die 2 "--kind에 값이 필요합니다."; kind=$2; shift 2 ;;
            --body) [[ $# -ge 2 ]] || die 2 "--body에 파일이 필요합니다."; body=$2; shift 2 ;;
            --milestone) [[ $# -ge 2 ]] || die 2 "--milestone에 이름이 필요합니다."; milestone=$2; shift 2 ;;
            --no-packages) no_packages=1; shift ;;
            --dry-run) DRY_RUN=1; shift ;;
            --*) die 2 "알 수 없는 start 옵션입니다: $1" ;;
            *) [[ -z "$title" ]] || die 2 "제목은 하나만 지정할 수 있습니다."; title=$1; shift ;;
        esac
    done

    if [[ -n "$issue_arg" ]]; then
        [[ "$issue_arg" =~ ^[1-9][0-9]*$ ]] || die 2 "Issue 번호가 올바르지 않습니다: $issue_arg"
        [[ -z "$title" && -z "$area" && -z "$kind" && -z "$body" && -z "$milestone" ]] \
            || die 2 "--issue 방식에는 제목, --area, --kind, --body, --milestone을 함께 쓸 수 없습니다."
        load_issue "$issue_arg"
        area=$(issue_label_value "$ISSUE_NUMBER" 'area:' || true)
        [[ -n "$area" ]] || die 1 "Issue #$ISSUE_NUMBER에서 area 라벨을 찾지 못했습니다."
        validate_area "$area"
    else
        [[ -n "$title" && -n "$area" && -n "$kind" && -n "$body" ]] \
            || die 2 "새 Issue에는 제목, --area, --kind, --body가 모두 필요합니다."
        validate_area "$area"
        validate_kind "$kind"
        validate_issue_body "$body"

        found=$(find_issue_by_title "$title" || true)
        if [[ -n "$found" ]]; then
            IFS=$'\t' read -r found_number _found_url <<<"$found"
            load_issue "$found_number"
            [[ "$(issue_label_value "$ISSUE_NUMBER" 'area:' || true)" == "$area" ]] \
                || die 1 "같은 제목의 Issue #$ISSUE_NUMBER가 다른 area에 있습니다."
            existing_kind=$(issue_label_value "$ISSUE_NUMBER" 'kind:' || true)
            [[ -z "$existing_kind" || "$existing_kind" == "$kind" ]] \
                || die 1 "같은 제목의 Issue #$ISSUE_NUMBER가 다른 kind에 있습니다: $existing_kind"
            printf 'Issue 재사용: #%s (%s)\n' "$ISSUE_NUMBER" "$ISSUE_URL"
        elif ((DRY_RUN)); then
            print_command gh issue create --title "$title" --body-file "$body" --label "area: $area" --label "kind: $kind"
            printf '새 Issue 번호는 실행 뒤 정해지므로 이후 단계는 이번 dry-run에서 실행하지 않습니다.\n'
            printf '재개 명령: %s\n' "$RESUME_COMMAND"
            return 0
        else
            ISSUE_URL=$(run_mutation_capture gh issue create --title "$title" --body-file "$body" \
                --label "area: $area" --label "kind: $kind")
            ISSUE_URL=$(printf '%s\n' "$ISSUE_URL" | tail -n 1)
            ISSUE_NUMBER=${ISSUE_URL##*/}
            [[ "$ISSUE_NUMBER" =~ ^[1-9][0-9]*$ ]] || die 1 "생성된 Issue 번호를 URL에서 읽지 못했습니다: $ISSUE_URL"
            ISSUE_TITLE=$title
            printf 'Issue 생성: #%s (%s)\n' "$ISSUE_NUMBER" "$ISSUE_URL"
        fi

        existing_kind=$(issue_label_value "$ISSUE_NUMBER" 'kind:' || true)
        if [[ -z "$existing_kind" ]]; then
            run_mutation gh issue edit "$ISSUE_NUMBER" --add-label "kind: $kind"
        fi
    fi

    [[ -n "$milestone" ]] || milestone=$(default_milestone "$root")
    set_milestone_best_effort "$ISSUE_NUMBER" "$milestone"

    slug=$(slugify "$ISSUE_TITLE")
    WORK_BRANCH=$(existing_branch_for_issue "$ISSUE_NUMBER" || true)
    if [[ -n "$WORK_BRANCH" && "$WORK_BRANCH" != "$area/$ISSUE_NUMBER-$slug" ]]; then
        warn "기존 Issue 브랜치 '$WORK_BRANCH'가 canonical 이름과 다르지만 재사용합니다."
    fi
    [[ -n "$WORK_BRANCH" ]] || WORK_BRANCH="$area/$ISSUE_NUMBER-$slug"
    target="${HOME}/project/zlink-$ISSUE_NUMBER-$slug"
    ensure_worktree "$root" "$WORK_BRANCH" "$target"

    if ((no_packages)); then
        printf '패키지 준비 건너뜀 (--no-packages)\n'
    else
        prepare_packages "$WORKTREE_PATH"
    fi
    project_status_best_effort "$ISSUE_URL" 'In Progress'

    printf '시작 완료: Issue #%s\n' "$ISSUE_NUMBER"
    printf '브랜치: %s\n' "$WORK_BRANCH"
    printf 'worktree: %s\n' "$WORKTREE_PATH"
}

branch_issue_number() {
    local branch=$1
    if [[ "$branch" =~ ^[^/]+/([1-9][0-9]*)-[a-z0-9-]+$ ]]; then
        printf '%s\n' "${BASH_REMATCH[1]}"
    else
        return 1
    fi
}

validate_pr_body() {
    local body=$1 issue=$2 mode=$3 first expected
    require_file "$body"
    first=$(sed -n '1{s/\r$//;p;}' "$body")
    if [[ "$mode" == closes ]]; then expected="Closes #$issue"; else expected="Refs #$issue"; fi
    [[ "$first" == "$expected" ]] || die 2 "PR 본문 첫 줄은 '$expected'여야 합니다."
    grep -Eq '^[[:space:]]{0,3}#{1,6}[[:space:]]+검증([[:space:]:—-]|$)' "$body" \
        || die 2 "PR 본문에 '검증' 절이 필요합니다."
}

remote_branch_sha() {
    local branch=$1
    git ls-remote --heads origin "refs/heads/$branch" | awk 'NR == 1 { print $1 }'
}

pr_command() {
    local body="" mode=refs mode_seen=0 branch issue title local_sha remote_sha pr_number base pr_url root
    root=$(repo_root)
    while (($#)); do
        case "$1" in
            --body) [[ $# -ge 2 ]] || die 2 "--body에 파일이 필요합니다."; body=$2; shift 2 ;;
            --closes) ((mode_seen == 0)) || die 2 "--closes와 --refs는 한 번만 쓸 수 있습니다."; mode=closes; mode_seen=1; shift ;;
            --refs) ((mode_seen == 0)) || die 2 "--closes와 --refs는 한 번만 쓸 수 있습니다."; mode=refs; mode_seen=1; shift ;;
            --dry-run) DRY_RUN=1; shift ;;
            *) die 2 "알 수 없는 pr 인자입니다: $1" ;;
        esac
    done
    [[ -n "$body" ]] || die 2 "pr에는 --body <파일>이 필요합니다."
    branch=$(current_branch)
    WORK_BRANCH=$branch
    issue=$(branch_issue_number "$branch") || die 2 "현재 브랜치가 Issue 브랜치 형식이 아닙니다: $branch"
    ISSUE_NUMBER=$issue
    WORKTREE_PATH=$root
    validate_pr_body "$body" "$issue" "$mode"

    title=$(git log -1 --pretty=%s)
    [[ "$title" =~ ^[^:[:space:]][^:]*:[[:space:]]+.+$ ]] \
        || die 2 "최신 커밋 제목이 '<모듈>: <요약>' 형식이 아닙니다: $title"
    local_sha=$(git rev-parse HEAD)
    remote_sha=$(remote_branch_sha "$branch")
    if [[ "$remote_sha" != "$local_sha" ]]; then
        run_mutation git push -u origin "$branch"
    else
        printf 'push 건너뜀: 원격 브랜치가 HEAD와 같습니다.\n'
    fi

    pr_number=$(gh pr list --head "$branch" --state open --limit 1 --json number --jq '.[0].number // empty')
    if [[ -n "$pr_number" ]]; then
        base=$(gh pr view "$pr_number" --json baseRefName --jq '.baseRefName')
        [[ "$base" == main ]] || die 1 "기존 PR #$pr_number의 base가 main이 아닙니다: $base"
        run_mutation gh pr edit "$pr_number" --title "$title" --body-file "$body" --base main
        pr_url=$(gh pr view "$pr_number" --json url --jq '.url')
        printf 'PR 갱신: #%s (%s)\n' "$pr_number" "$pr_url"
    else
        if ((DRY_RUN)); then
            print_command gh pr create --base main --head "$branch" --title "$title" --body-file "$body"
            pr_url='<새 PR URL>'
        else
            pr_url=$(run_mutation_capture gh pr create --base main --head "$branch" --title "$title" --body-file "$body")
            pr_url=$(printf '%s\n' "$pr_url" | tail -n 1)
        fi
        printf 'PR 생성: %s\n' "$pr_url"
    fi

    load_issue "$issue"
    project_status_best_effort "$ISSUE_URL" 'Review'
}

ensure_closes_cleanup_safe() {
    local worktree=$1 branch=$2 state=$3 local_sha remote_sha
    [[ -z "$(git -C "$worktree" status --porcelain)" ]] \
        || die 1 "worktree에 커밋되지 않은 변경이 있어 정리할 수 없습니다."
    local_sha=$(git -C "$worktree" rev-parse "$branch")
    remote_sha=$(remote_branch_sha "$branch")
    if [[ -z "$remote_sha" ]]; then
        [[ "$state" == MERGED ]] || die 1 "원격 브랜치가 없어 push 여부를 검증할 수 없습니다."
    elif [[ "$local_sha" != "$remote_sha" ]]; then
        die 1 "원격에 push되지 않은 커밋이 있어 정리할 수 없습니다."
    fi
}

done_command() {
    local verified="" branch issue pr_number state head_oid base first_line mode worktree primary remote_sha
    while (($#)); do
        case "$1" in
            --verified) [[ $# -ge 2 ]] || die 2 "--verified에 SHA가 필요합니다."; verified=$2; shift 2 ;;
            --dry-run) DRY_RUN=1; shift ;;
            *) die 2 "알 수 없는 done 인자입니다: $1" ;;
        esac
    done
    [[ "$verified" =~ ^[0-9a-fA-F]{40}$ ]] || die 2 "--verified에는 40자리 commit SHA가 필요합니다."
    verified=${verified,,}

    branch=$(current_branch)
    WORK_BRANCH=$branch
    issue=$(branch_issue_number "$branch") || die 2 "현재 브랜치가 Issue 브랜치 형식이 아닙니다: $branch"
    ISSUE_NUMBER=$issue
    worktree=$(repo_root)
    WORKTREE_PATH=$worktree
    pr_number=$(gh pr list --head "$branch" --state all --limit 1 --json number --jq '.[0].number // empty')
    [[ -n "$pr_number" ]] || die 1 "브랜치 '$branch'의 PR을 찾지 못했습니다."
    state=$(gh pr view "$pr_number" --json state --jq '.state')
    head_oid=$(gh pr view "$pr_number" --json headRefOid --jq '.headRefOid')
    base=$(gh pr view "$pr_number" --json baseRefName --jq '.baseRefName')
    first_line=$(gh pr view "$pr_number" --json body --jq '.body | split("\n")[0]')
    [[ "$base" == main ]] || die 1 "PR #$pr_number의 base가 main이 아닙니다: $base"
    [[ "${head_oid,,}" == "$verified" ]] \
        || die 1 "검증 SHA($verified)와 PR HEAD(${head_oid,,})가 다릅니다."

    case "$first_line" in
        "Closes #$issue") mode=closes ;;
        "Refs #$issue") mode=refs ;;
        *) die 1 "PR 본문 첫 줄이 Issue #$issue를 Closes/Refs 하지 않습니다: $first_line" ;;
    esac

    if [[ "$mode" == closes ]]; then
        ensure_closes_cleanup_safe "$worktree" "$branch" "$state"
    fi

    if [[ "$state" == OPEN ]]; then
        run_mutation gh pr merge "$pr_number" --merge --match-head-commit "$verified"
        printf 'PR #%s merge 요청 완료\n' "$pr_number"
    elif [[ "$state" == MERGED ]]; then
        printf 'PR #%s는 이미 merge되었습니다.\n' "$pr_number"
    else
        die 1 "PR #$pr_number 상태가 merge 가능하지 않습니다: $state"
    fi

    if [[ "$mode" == refs ]]; then
        printf 'Refs PR: Issue와 worktree를 유지합니다.\n'
        return 0
    fi

    remote_sha=$(remote_branch_sha "$branch")
    if [[ -n "$remote_sha" ]]; then
        run_mutation git push origin --delete "$branch"
    else
        printf '원격 브랜치 삭제 건너뜀: 이미 없습니다.\n'
    fi

    primary=$(git worktree list --porcelain | awk '$1 == "worktree" { print substr($0, index($0, " ") + 1); exit }')
    [[ -n "$primary" ]] || die 1 "기본 worktree를 찾지 못했습니다."
    load_issue "$issue"
    if ((DRY_RUN)); then
        print_command git -C "$primary" worktree remove "$worktree"
    else
        cd "$primary"
        git worktree remove "$worktree"
        WORKTREE_PATH=""
    fi
    project_status_best_effort "$ISSUE_URL" 'Done'
    printf '정리 완료: 원격 브랜치와 worktree를 제거하고 Project Done 갱신을 시도했습니다.\n'
}

status_local_rows() {
    local path="" branch="" issue issue_state pr_cell pr_ref checks
    printf 'worktree\t브랜치\tIssue\tPR\tCI\n'
    while IFS= read -r line; do
        case "$line" in
            worktree\ *) path=${line#worktree } ;;
            branch\ refs/heads/*) branch=${line#branch refs/heads/} ;;
            '')
                if [[ -n "$path" && -n "$branch" ]]; then
                    issue=$(branch_issue_number "$branch" || true)
                    issue_state='-'; pr_cell='-'; checks='-'
                    if [[ -n "$issue" ]]; then
                        issue_state=$(gh issue view "$issue" --json state --jq '.state' 2>/dev/null || printf '조회 실패')
                        pr_cell=$(gh pr list --head "$branch" --state all --limit 1 --json number,state \
                            --jq 'if length == 0 then "-" else "#\(.[0].number) \(.[0].state)" end' 2>/dev/null || printf '조회 실패')
                        if [[ "$pr_cell" == \#* ]]; then
                            pr_ref=${pr_cell%% *}
                            pr_ref=${pr_ref#\#}
                            if ! checks=$(gh pr view "$pr_ref" --json statusCheckRollup --jq \
                                '.statusCheckRollup | if length == 0 then "없음" elif all(.[]; ((.conclusion // .state // "") == "SUCCESS" or (.conclusion // .state // "") == "SKIPPED")) then "PASS" elif any(.[]; ((.status // .state // "") == "PENDING" or (.status // .state // "") == "QUEUED" or (.status // .state // "") == "IN_PROGRESS")) then "대기" else "실패" end' \
                                2>/dev/null); then
                                [[ -n "$checks" ]] || checks='조회 실패'
                            fi
                        fi
                    fi
                    printf '%s\t%s\t%s%s\t%s\t%s\n' "$path" "$branch" \
                        "${issue:+#$issue }" "$issue_state" "$pr_cell" "$checks"
                fi
                path=''; branch='' ;;
        esac
    done < <(git worktree list --porcelain; printf '\n')
}

status_milestone() {
    local milestone=$1 rows number title state url pr_rows pr_number merge_oid body_first included overall=0
    if ! rows=$(gh issue list --milestone "$milestone" --state all --limit 100 \
        --json number,title,state,url --jq '.[] | [.number,.title,.state,.url] | @tsv' 2>/dev/null); then
        warn "milestone '$milestone' 조회에 실패했습니다."
        return 1
    fi
    printf '\nmilestone %s 릴리스 조건\n' "$milestone"
    printf 'Issue\t상태\tCloses merge PR\torigin/main 포함\n'
    while IFS=$'\t' read -r number title state url; do
        [[ -n "$number" ]] || continue
        pr_number=''; merge_oid=''; included='아니요'
        pr_rows=$(gh pr list --state merged --search "#$number" --limit 100 \
            --json number,mergeCommit,body --jq '.[] | [.number,.mergeCommit.oid,(.body | split("\n")[0])] | @tsv' 2>/dev/null || true)
        while IFS=$'\t' read -r candidate oid body_first; do
            if [[ "$body_first" == "Closes #$number" ]]; then
                pr_number=$candidate; merge_oid=$oid; break
            fi
        done <<<"$pr_rows"
        if [[ -n "$merge_oid" ]] && git merge-base --is-ancestor "$merge_oid" origin/main 2>/dev/null; then
            included='예'
        fi
        [[ "$state" == CLOSED && -n "$pr_number" && "$included" == '예' ]] || overall=1
        printf '#%s\t%s\t%s\t%s\n' "$number" "$state" "${pr_number:+#$pr_number}" "$included"
    done <<<"$rows"
    if ((overall == 0)); then
        printf '판정: 릴리스 조건 충족\n'
    else
        printf '판정: 릴리스 조건 미충족\n'
    fi
    return "$overall"
}

status_command() {
    local milestone="" status_code=0
    while (($#)); do
        case "$1" in
            --milestone) [[ $# -ge 2 ]] || die 2 "--milestone에 이름이 필요합니다."; milestone=$2; shift 2 ;;
            --dry-run) DRY_RUN=1; shift ;;
            *) die 2 "알 수 없는 status 인자입니다: $1" ;;
        esac
    done
    repo_root >/dev/null
    status_local_rows
    if [[ -n "$milestone" ]]; then
        status_milestone "$milestone" || status_code=$?
    fi
    return "$status_code"
}

main() {
    local original=("$0" "$@") args=() command arg
    require_command git
    require_command gh
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
    set -- "${args[@]:1}"
    case "$command" in
        start) start_command "$@" ;;
        pr) pr_command "$@" ;;
        status) status_command "$@" || exit $? ;;
        done) done_command "$@" ;;
        -h|--help|help) usage ;;
        *) usage; die 2 "알 수 없는 명령입니다: $command" ;;
    esac
}

main "$@"
