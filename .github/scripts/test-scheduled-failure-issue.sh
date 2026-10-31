#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "$script_dir/scheduled-failure-issue.sh"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

assert_duplicate_number() {
  local issues_json="$1"
  local expected_number="$2"
  local label_exists="$3"
  local match

  match="$(find_matching_issue "$issues_json" "$title" "$label" "$label_exists")"
  [[ -n "$match" ]] || fail "expected duplicate issue #$expected_number"
  [[ "$(jq -r '.number' <<< "$match")" == "$expected_number" ]] || fail "expected duplicate issue #$expected_number, got $match"
}

assert_no_duplicate() {
  local issues_json="$1"
  local label_exists="$2"
  local match

  match="$(find_matching_issue "$issues_json" "$title" "$label" "$label_exists")"
  [[ -z "$match" ]] || fail "expected a new issue, got duplicate $match"
}

title="nightly: Framework Node 실패"
label="nightly-failure"

same_title_with_label='[{"number":101,"title":"nightly: Framework Node 실패","labels":[{"name":"nightly-failure"}]}]'
assert_duplicate_number "$same_title_with_label" 101 true
printf 'PASS: same title and matching label is a duplicate\n'

same_title_with_other_label='[{"number":102,"title":"nightly: Framework Node 실패","labels":[{"name":"other-label"}]}]'
assert_no_duplicate "$same_title_with_other_label" true
printf 'PASS: same title and different label is new\n'

same_title_without_repository_label='[{"number":103,"title":"nightly: Framework Node 실패","labels":[]}]'
assert_duplicate_number "$same_title_without_repository_label" 103 false
printf 'PASS: same title is a duplicate when the repository label is unavailable\n'

different_title='[{"number":104,"title":"nightly: Framework Java 실패","labels":[{"name":"nightly-failure"}]}]'
assert_no_duplicate "$different_title" true
printf 'PASS: different title is new\n'

body="$(build_issue_body 'Framework Node' 'https://example.test/runs/1' 'abc123' 'failure' '- unit' '')"
case "$body" in
  [[:space:]]*) fail 'issue body starts with whitespace' ;;
esac
while IFS= read -r line; do
  case "$line" in
    [[:space:]]*) fail 'issue body contains a line with leading whitespace' ;;
  esac
done <<< "$body"
printf 'PASS: issue body starts without whitespace and has no indented lines\n'
