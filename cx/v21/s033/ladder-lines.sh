#!/bin/bash
# Prints one ladder line per language from live job status (no hand-edited ETAs).
B=/root/jobs/fx/cx/v21/s033
st=$(cd /root/zlink && bash scripts/dev/job.sh status 2>/dev/null)
while IFS='|' read -r lang stage job; do
  [ -z "$lang" ] && continue
  row=$(printf '%s\n' "$st" | awk -F'\t' -v j="$job" '$1==j')
  state=$(printf '%s' "$row" | cut -f5); age=$(printf '%s' "$row" | cut -f4)
  verdict=$(grep -m1 "^$lang " $B/ladder-status.txt 2>/dev/null | cut -d' ' -f2-)
  wt=$(printf '%s' "$row" | cut -f3)
  lockinfo=""
  while read -r pid mode path; do
    cwd=$(readlink /proc/$pid/cwd 2>/dev/null)
    case "$cwd" in "$wt"|"$wt"/*) ;; *) continue;; esac
    mins=$(( $(ps -o etimes= -p $pid 2>/dev/null || echo 0) / 60 ))
    case "$path:$mode" in
      *measure.lock:WRITE) lockinfo="측정 중 ${mins}분째";;
      *measure.lock:WRITE\*) lockinfo="측정 lock 대기 ${mins}분째";;
      *measure.gate:*) [ -z "$lockinfo" ] && lockinfo="빌드·시험이 측정 gate 대기 ${mins}분째";;
    esac
  done < <(lslocks -n -o PID,MODE,PATH 2>/dev/null | grep -E "measure\.(lock|gate)")
  if [ "$state" = "실행" ]; then s="실행 중 ($age)${lockinfo:+, $lockinfo}"
  elif [ -s $B/$job/report.md ]; then s="**끝남 — 보고서 있음, kairos 판정 대기**"
  else s="**멈춤 — 보고서 없이 끝남 ($state)**"; fi
  echo "$lang $stage: $s${verdict:+ · 직전 판정: $verdict}"
done < $B/ladder-jobs.txt
