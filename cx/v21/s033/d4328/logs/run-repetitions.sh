#!/usr/bin/env bash
set -u
binary=/root/worktree/zlink-v21-s033/d4328/build-d4328/bin/test_socket_disconnect_progress_without_app_poll
logdir=/root/jobs/fx/cx/v21/s033/d4328/logs
mode=$1
start=$2
end=$3
for n in $(seq "$start" "$end"); do
  id=$(printf '%s-%03d' "$mode" "$n")
  out="$logdir/$id.log"
  source /root/jobs/fx/cx/measure-gate.sh
  measure_gate_pass
  stress_pid=
  if [ "$mode" = "load" ]; then
    stress-ng --cpu 16 --timeout 20s --quiet > "$logdir/$id.stress.log" 2>&1 &
    stress_pid=$!
  fi
  flock -s /root/jobs/fx/cx/measure.lock bash -c 'exec env ZLINK_TEST_CASE=test_tcp_registered_without_waiter_progresses "$1"' _ "$binary" > "$out" 2>&1 &
  pid=$!
  elapsed=0
  while kill -0 "$pid" 2>/dev/null && [ "$elapsed" -lt 8 ]; do
    sleep 1
    elapsed=$((elapsed + 1))
  done
  if kill -0 "$pid" 2>/dev/null; then
    echo "HUNG mode=$mode run=$n pid=$pid elapsed=${elapsed}s" | tee -a "$logdir/run-status.log"
    child=$(pgrep -P "$pid" | head -n 1)
    target=${child:-$pid}
    echo "GDB_TARGET mode=$mode run=$n pid=$target parent=$pid" | tee -a "$logdir/run-status.log"
    gdb -q -batch -p "$target" -ex 'set pagination off' -ex 'thread apply all bt' > "$logdir/$id.gdb.txt" 2>&1
    kill -TERM "$target"
  fi
  wait "$pid"
  rc=$?
  if [ -n "$stress_pid" ]; then kill -TERM "$stress_pid" 2>/dev/null || true; wait "$stress_pid" 2>/dev/null || true; fi
  echo "RUN mode=$mode run=$n rc=$rc elapsed=${elapsed}s" | tee -a "$logdir/run-status.log"
done
