#!/usr/bin/env bash
# Per-cell judgement: screen at runs=1; if FAIL, confirm once at runs=3.
# usage: percell.sh <single|multi> P:T [P:T ...]
export ZLINK_WORK=$HOME/project/zlink-work
suite=$1; shift
R=$ZLINK_WORK/c016/sweep2-results.md
V=$ZLINK_WORK/c016/percell-verdicts.log
cd ~/project/zlink
verdict() { grep -E "^\| $1 \| $2 \| $3 \|" $R | tail -1 | awk -F'|' '{gsub(/ /,"",$5); print $5}'; }
for cell in "$@"; do
  p=${cell%%:*}; t=${cell#*:}
  SWEEP2_RUNS=1 bash $ZLINK_WORK/c016/tools/sweep2.sh --only $suite --cells $cell --retry-failed >/dev/null 2>&1
  v1=$(verdict $suite $p $t)
  if [[ "$v1" == FAIL ]]; then
    SWEEP2_RUNS=3 bash $ZLINK_WORK/c016/tools/sweep2.sh --only $suite --cells $cell --retry-failed >/dev/null 2>&1
    v2=$(verdict $suite $p $t)
    echo "CELL $suite $cell screen=FAIL confirm(runs3)=$v2 :: $(grep -E "^\| $suite \| $p \| $t \|" $R | tail -1 | cut -d'|' -f6,7,10)" >> $V
  else
    echo "CELL $suite $cell screen=$v1 :: $(grep -E "^\| $suite \| $p \| $t \|" $R | tail -1 | cut -d'|' -f6,7,10)" >> $V
  fi
done
echo "BATCH_DONE $suite $*" >> $V
