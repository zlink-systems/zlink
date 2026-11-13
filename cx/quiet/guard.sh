#!/bin/bash
# Machine-quiet guard for measurements (PM 2026-11-02; tightened 2026-11-13 after request-serial spread).
# While a process holds measure.lock exclusive, everything not descended from the holder must be quiet:
#  - builds AND tests of every language are paused (SIGSTOP) and resumed on release;
#  - every other agent process tree (codex jobs, the kairos session's helpers) runs SCHED_IDLE,
#    so it cannot preempt the measured threads; restored to SCHED_OTHER on release.
# Each pause is logged with the holder PID so a run that overlapped can be found.
L=/root/jobs/fx/cx/measure.lock; LOG=/root/jobs/fx/cx/quiet/guard.log
BUILD='stress-ng|cc1plus|/cc1 |collect2|bin/ld |ld\.(bfd|gold) |/ninja|ninja -j|cmake --build|cmake -S|MSBuild\.dll|VBCSCompiler|dotnet (build|msbuild|publish|pack|restore|test)|GradleDaemon|GradleWorkerMain|GradleWrapperMain|gradlew|javac|kotlinc|/tsc( |$)|esbuild|npm (run )?(build|test)|node --test|ctest|testhost|vstest|_test( |$)|protoc'
AGENTS='codex exec|codex-code-|claude --'
stopped=(); idled=()
desc() { local p=$1; echo $p; for c in $(pgrep -P $p); do desc $c; done; }
resume() {
  ((${#stopped[@]})) && { kill -CONT "${stopped[@]}" 2>/dev/null; echo "$(date -u +%FT%TZ) resume ${#stopped[@]}" >> $LOG; }
  for p in "${idled[@]}"; do chrt -a -o -p 0 $p 2>/dev/null; done
  stopped=(); idled=()
}
trap 'resume; exit 0' TERM INT
# The guard and everything it forks run SCHED_IDLE: it must never preempt a measured thread.
chrt -a -i -p 0 $$
while :; do
  h=$(lslocks -n -o PID,MODE,PATH | awk -v l=$L '$3==l && $2=="WRITE"{print $1; exit}')
  if [ -n "$h" ]; then
    declare -A mine=(); for p in $(desc $h); do mine[$p]=1; done
    mine[$$]=1
    # the holder's ancestors (its own job shell/codex) stay as they are
    a=$h; while [ -n "$a" ] && [ "$a" -gt 1 ]; do mine[$a]=1; a=$(awk '{print $4}' /proc/$a/stat 2>/dev/null); done
    while read -r pid args; do
      [ -n "${mine[$pid]}" ] && continue
      if [[ " ${stopped[*]} " != *" $pid "* ]] && kill -STOP $pid 2>/dev/null; then stopped+=($pid); echo "$(date -u +%FT%TZ) holder=$h stop $pid ${args:0:120}" >> $LOG; fi
    done < <(ps -eo pid=,args= | grep -E "$BUILD" | grep -vE 'grep|guard\.sh|flock ')
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
