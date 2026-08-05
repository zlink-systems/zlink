#!/usr/bin/env bash

run_node_job() {
  local timeout_sec="$1"
  shift
  setsid "$@" &
  local job_pid=$!
  local watcher_pid=""
  local rc=0

  (
    sleep "$timeout_sec"
    if kill -0 "$job_pid" 2>/dev/null; then
      kill -TERM -- "-$job_pid" 2>/dev/null || true
      sleep 2
      kill -KILL -- "-$job_pid" 2>/dev/null || true
    fi
  ) &
  watcher_pid=$!

  if wait "$job_pid"; then
    rc=0
  else
    rc=$?
  fi

  kill "$watcher_pid" 2>/dev/null || true
  wait "$watcher_pid" 2>/dev/null || true
  kill -TERM -- "-$job_pid" 2>/dev/null || true

  return "$rc"
}
