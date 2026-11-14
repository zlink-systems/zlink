#!/usr/bin/env bash
set -euo pipefail
job=/root/jobs/fx/cx/v21/s033/lfx-java
old=$1 new=$2 group=$3
case "$group" in
  1) cells=("send-saturation 1024" "send-saturation 4096" "request-serial 1024");;
  2) cells=("request-serial 4096" "request-backpressure 1024" "request-backpressure 4096");;
  *) echo 'group must be 1 or 2' >&2; exit 2;;
esac
wait_start=$(date -u +%FT%TZ); start_s=$(date +%s); waited=0
printf -- '- %s %s-%s group%s build wait started; wait up to 600 s for active cmake/ninja/cc1plus/gradlew/dotnet build/tsc/npm run build processes before acquiring measure.lock\n' "$wait_start" "$old" "$new" "$group" >> "$job/progress.md"
while (( waited < 600 )); do
  if ! ps -eo stat=,args= | awk '$1 ~ /^[RS]/ && $0 ~ /(cmake --build|ninja|cc1plus|gradlew|dotnet build|tsc|npm run build)/ {found=1} END{exit !found}'; then break; fi
  sleep 1; waited=$(( $(date +%s) - start_s ));
done
waited=$(( $(date +%s) - start_s ));
printf -- '- %s-%s group%s build wait ended after %ss; active builder remains=%s\n' "$old" "$new" "$group" "$waited" "$(ps -eo stat=,args= | awk '$1 ~ /^[RS]/ && $0 ~ /(cmake --build|ninja|cc1plus|gradlew|dotnet build|tsc|npm run build)/ {n++} END{print n+0}')" >> "$job/progress.md"
exec 9>/root/jobs/fx/cx/measure.lock
flock -x 9
printf 'hold_start_utc=%s steps=%s-%s group=%s\n' "$(date -u +%FT%TZ)" "$old" "$new" "$group" >> "$job/raw/holds.txt"
trap 'printf "hold_end_utc=%s steps=%s-%s group=%s\n" "$(date -u +%FT%TZ)" "$old" "$new" "$group" >> "$job/raw/holds.txt"' EXIT
for item in "${cells[@]}"; do
  read -r pattern size <<< "$item"
  for step in "$old" "$new"; do
    RAW_VARIANT=attempt3 bash "$job/tools/run-one.sh" "$step" "$pattern" "$size" warm
    RAW_VARIANT=attempt3 bash "$job/tools/run-one.sh" "$step" "$pattern" "$size" counted-1
  done
  old_rate=$(awk -v o="$old" -v p="$pattern" -v s="$size" '$1==o && $2==p && $3==s && $4=="counted-1" && $5=="yes" {r=$6} END{print r}' "$job/raw/attempt3/index.tsv")
  new_rate=$(awk -v o="$new" -v p="$pattern" -v s="$size" '$1==o && $2==p && $3==s && $4=="counted-1" && $5=="yes" {r=$6} END{print r}' "$job/raw/attempt3/index.tsv")
  loss=$(awk -v a="$old_rate" -v b="$new_rate" 'BEGIN{if(a>0) printf "%.6f",1-b/a; else print 99}')
  if awk -v x="$loss" 'BEGIN{exit !(x>=0.05 && x<=0.09)}'; then
    printf -- '- %s-%s %s-%s first-pair loss=%s%% in 5–9%% boundary; adding two counted pairs inside this hold\n' "$old" "$new" "$pattern" "$size" "$(awk -v x="$loss" 'BEGIN{printf "%.2f",x*100}')" >> "$job/progress.md"
    for n in 2 3; do
      for step in "$old" "$new"; do
        RAW_VARIANT=attempt3 bash "$job/tools/run-one.sh" "$step" "$pattern" "$size" "warm-extra$n"
        RAW_VARIANT=attempt3 bash "$job/tools/run-one.sh" "$step" "$pattern" "$size" "counted-$n"
      done
    done
  fi
done
