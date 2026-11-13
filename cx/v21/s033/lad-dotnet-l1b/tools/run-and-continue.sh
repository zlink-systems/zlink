#!/usr/bin/env bash
set -euo pipefail
session=/root/jobs/fx/cx/v21/s033/lad-dotnet-l1b/tools/run-complete-session.sh
"$session" "$@" &
root=$!
resume_tree(){
  local parent="$1" child
  kill -CONT "$parent" 2>/dev/null || true
  while read -r child; do
    [[ -n "$child" ]] && resume_tree "$child"
  done < <(pgrep -P "$parent" || true)
}
while kill -0 "$root" 2>/dev/null; do
  resume_tree "$root"
  sleep 1
done
wait "$root"
