#!/bin/bash
# Machine-quiet guard for measurements (PM 2026-11-02; tightened 2026-11-13 after request-serial spread).
# While a process holds measure.lock exclusive, everything not descended from the holder must be quiet:
#  - builds AND tests of every language are paused (SIGSTOP) and resumed on release;
#  - every other agent process tree (codex jobs, the kairos session's helpers) runs SCHED_IDLE,
#    so it cannot preempt the measured threads; restored to SCHED_OTHER on release.
# Each pause is logged with the holder PID so a run that overlapped can be found.
L=/root/jobs/fx/cx/measure.lock; LOG=/root/jobs/fx/cx/quiet/guard.log
BUILD='stress-ng|cc1plus|/cc1 |collect2|bin/ld |ld\.(bfd|gold) |/ninja|ninja -j|cmake --build|cmake -S|MSBuild\.dll|VBCSCompiler|dotnet (build|msbuild|publish|pack|restore|test)|GradleDaemon|GradleWorkerMain|GradleWrapperMain|gradlew|javac|kotlinc|/tsc( |$)|esbuild|npm (run )?(build|test)|node --test|ctest|testhost|vstest|_test( |$)|protoc|(^| |/)(tar|zstd|xz|pigz|gzip|du|bfs|find|rg|rsync|sha256sum)( |$)|git (gc|push|fetch|pull|clone|repack|fsck|archive)'
AGENTS='codex exec|codex-code-|claude --'
stopped=(); idled=(); cdeep=0
# Deep C-states (C2 18 us, C3 350 us exit latency on this Ryzen 7 5825U) add wake-up latency to
# 1:1 request-serial round trips (~90 us). Disabled on all CPUs while a measurement holds the lock
# (aa-noise3 11-13: request-serial A/A G5 32%).
cstates() { local v=$1 f; for f in /sys/devices/system/cpu/cpu*/cpuidle/state[23]/disable; do echo $v > $f 2>/dev/null; done; cdeep=$v; echo "$(date -u +%FT%TZ) deep-cstates-disabled=$v" >> $LOG; }
desc() { local p=$1; echo $p; for c in $(pgrep -P $p); do desc $c; done; }
resume() {
  ((${#stopped[@]})) && { kill -CONT "${stopped[@]}" 2>/dev/null; echo "$(date -u +%FT%TZ) resume ${#stopped[@]}" >> $LOG; }
  for p in "${idled[@]}"; do chrt -a -o -p 0 $p 2>/dev/null; done
  stopped=(); idled=()
  ((cdeep)) && cstates 0
}
trap 'resume; exit 0' TERM INT
# The guard and everything it forks run SCHED_IDLE: it must never preempt a measured thread.
chrt -a -i -p 0 $$
while :; do
  # queued measurements too: a waiter forked by an idled agent would start its runs SCHED_IDLE
  for w in $(awk '$2=="->" && $5=="WRITE" && $7 ~ /:1237411609$/ {print $6}' /proc/locks); do for p in $(desc $w); do chrt -a -o -p 0 $p 2>/dev/null; done; done
  h=$(lslocks -n -o PID,MODE,PATH | awk -v l=$L '$3==l && $2=="WRITE"{print $1; exit}')
  if [ -n "$h" ]; then
    declare -A mine=(); for p in $(desc $h); do mine[$p]=1; done
    ((cdeep)) || cstates 1
    # the measurement itself must run SCHED_OTHER: its waiter was forked by an agent that an
    # earlier hold set SCHED_IDLE, and children inherit that (A/A + Java m1 ran IDLE, 11-13 16:3x)
    for p in $h $(desc $h); do chrt -a -o -p 0 $p 2>/dev/null; done
    mine[$$]=1
    # the holder's ancestors (its own job shell/codex) stay as they are
    a=$h; while [ -n "$a" ] && [ "$a" -gt 1 ]; do mine[$a]=1; a=$(awk '{print $4}' /proc/$a/stat 2>/dev/null); done
    while read -r pid args; do
      [ -n "${mine[$pid]}" ] && continue
      if [[ " ${stopped[*]} " != *" $pid "* ]] && kill -STOP $pid 2>/dev/null; then stopped+=($pid); echo "$(date -u +%FT%TZ) holder=$h stop $pid ${args:0:120}" >> $LOG; fi
    done < <(ps -eo pid=,args= | grep -E "$BUILD" | grep -vE 'grep|guard\.sh|flock |dc\.ts|dchat ')
    for root in $(ps -eo pid=,args= | grep -E "$AGENTS" | grep -v grep | awk '{print $1}'); do
      for p in $(desc $root); do
        [ -n "${mine[$p]}" ] && continue
        [[ " ${idled[*]} " == *" $p "* ]] && continue
        chrt -a -i -p 0 $p 2>/dev/null && idled+=($p)
      done
    done
    unset mine
  else
    resume
  fi
  sleep 1
done
