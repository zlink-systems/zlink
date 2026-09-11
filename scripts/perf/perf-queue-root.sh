#!/usr/bin/env bash
# perf 큐의 기본 위치. 모든 linked worktree는 common Git directory의 부모(주 worktree)를 공유한다.

perf_queue_root() {
  local script_dir repo common_dir primary_worktree
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  repo="$(git -C "${script_dir}" rev-parse --show-toplevel)"
  common_dir="$(git -C "${repo}" rev-parse --path-format=absolute --git-common-dir)"
  primary_worktree="$(dirname "${common_dir}")"
  printf '%s\n' "${ZLINK_PERF_QUEUE:-${primary_worktree}/.artifacts/perf-queue}"
}
