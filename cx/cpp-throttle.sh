#!/usr/bin/env bash
# Machine-wide C++ compile throttle: keeps at most 5 C++ build trees (ninja/make under /root/worktree) running.
# Extra trees are paused with SIGSTOP (not killed) and resumed with SIGCONT, oldest first, when a slot frees.
# Only compile trees are paused (ninja + its cc1plus/c++ children); test executables are never touched.
MAX=5
tree(){ echo $1; for c in $(pgrep -P $1); do tree $c; done; }
while true; do
  mapfile -t builds < <(ps -eo pid=,etimes=,stat=,args= | awk '$4 ~ /(^|\/)ninja$/ {print $1, $2, $3}' | sort -k2,2nr)
  running=(); stopped=()
  for b in "${builds[@]}"; do set -- $b; if [[ $3 == T* ]]; then stopped+=($1); else running+=($1); fi; done
  # pause youngest running beyond MAX
  if (( ${#running[@]} > MAX )); then
    for pid in "${running[@]:MAX}"; do kill -STOP $(tree $pid) 2>/dev/null; echo "$(date +%T) paused $pid" ; done
  elif (( ${#running[@]} < MAX && ${#stopped[@]} > 0 )); then
    n=$(( MAX - ${#running[@]} ))
    for pid in "${stopped[@]:0:n}"; do kill -CONT $(tree $pid) 2>/dev/null; echo "$(date +%T) resumed $pid"; done
  fi
  sleep 15
done
