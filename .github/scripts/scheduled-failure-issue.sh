#!/usr/bin/env bash
set -euo pipefail

find_matching_issue() {
  local issues_json="$1"
  local expected_title="$2"
  local expected_label="$3"
  local label_exists="$4"

  jq -c --arg title "$expected_title" --arg label "$expected_label" --argjson label_exists "$label_exists" '
    first(
      .[]
      | select(.title == $title)
      | select((($label_exists | not) or any(.labels[]?; .name == $label)))
    ) // empty
  ' <<< "$issues_json"
}

build_issue_body() {
  local workflow_name="$1"
  local run_url="$2"
  local head_sha="$3"
  local run_conclusion="$4"
  local failed_job_lines="$5"
  local label_note="$6"
  local manual_unpause="수동 해제: 원인을 확인한 뒤 Mergify CLI에서 mergify queue unpause를 실행하거나 Mergify 대시보드에서 merge queue를 재개하세요."

  printf '%s\n\n예약 실행이 실패했습니다.\n- Workflow: %s\n- 실행: %s\n- Head SHA: %s\n- Conclusion: %s\n- 실패 job:\n%s%s' \
    "$manual_unpause" "$workflow_name" "$run_url" "$head_sha" "$run_conclusion" "$failed_job_lines" "$label_note"
}

build_comment_body() {
  local run_url="$1"
  local head_sha="$2"
  local failed_job_lines="$3"
  local label_note="$4"

  printf '예약 실행 실패가 다시 확인되었습니다.\n- 실행: %s\n- Head SHA: %s\n- 실패 job:\n%s%s' \
    "$run_url" "$head_sha" "$failed_job_lines" "$label_note"
}

main() {
  local title="nightly: ${WORKFLOW_NAME} 실패"
  local label="nightly-failure"
  local jobs_json failed_jobs failed_job_lines labels_json search_query open_issues existing
  local issue_number issue_url label_note label_state=unknown label_exists=false

  jobs_json="$(gh run view "$RUN_ID" --repo "$GH_REPOSITORY" --json jobs)"
  failed_jobs="$(jq -r '
    [.jobs[] | select(.conclusion == "failure" or .conclusion == "timed_out") | .name]
    | unique
    | if length == 0 then ["실패 job 이름을 찾지 못했습니다."] else . end
    | .[]
  ' <<< "$jobs_json")"
  failed_job_lines="$(sed 's/^/- /' <<< "$failed_jobs")"

  if labels_json="$(gh label list --repo "$GH_REPOSITORY" --limit 1000 --json name)"; then
    label_exists="$(jq -r --arg label "$label" 'any(.[]; .name == $label)' <<< "$labels_json")"
    if [[ "$label_exists" == true ]]; then
      label_state=present
    else
      label_state=missing
    fi
  else
    echo '::warning::Could not list repository labels; nightly-failure label availability is unknown.'
    label_exists=false
  fi

  if [[ "$label_state" == missing ]]; then
    echo '::warning::The nightly-failure label is missing; create it manually. This workflow will not create labels.'
  elif [[ "$label_state" == unknown ]]; then
    echo '::warning::Could not verify nightly-failure label availability; the issue will be recorded without a label.'
  fi

  search_query="\"${title}\" in:title"
  open_issues="$(gh issue list --repo "$GH_REPOSITORY" --state open --search "$search_query" --limit 1000 --json number,title,url,labels)"
  existing="$(find_matching_issue "$open_issues" "$title" "$label" "$label_exists")"

  label_note=""
  if [[ "$label_state" != present ]]; then
    label_note=$'\n\nnightly-failure 라벨 없이 기록했습니다. 저장소에서 라벨 상태를 확인하고, 없으면 생성해야 합니다.'
  fi

  if [[ -n "$existing" ]]; then
    issue_number="$(jq -r '.number' <<< "$existing")"
    issue_url="$(jq -r '.url' <<< "$existing")"
    comment="$(build_comment_body "$RUN_URL" "$HEAD_SHA" "$failed_job_lines" "$label_note")"
    gh issue comment "$issue_number" --repo "$GH_REPOSITORY" --body "$comment"
  else
    body="$(build_issue_body "$WORKFLOW_NAME" "$RUN_URL" "$HEAD_SHA" "$RUN_CONCLUSION" "$failed_job_lines" "$label_note")"
    if [[ "$label_exists" == true ]]; then
      issue_url="$(gh issue create --repo "$GH_REPOSITORY" --title "$title" --body "$body" --label "$label")"
    else
      issue_url="$(gh issue create --repo "$GH_REPOSITORY" --title "$title" --body "$body")"
    fi
  fi

  printf 'issue_url=%s\n' "$issue_url" >> "$GITHUB_OUTPUT"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  main "$@"
fi
