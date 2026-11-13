#!/bin/bash
# usage: resume.sh <jobdir> <worktree>  — resume the job's codex session; on "at capacity" wait and retry (blocking)
J=$1; W=$2
S=$(cat $J/codex*.log 2>/dev/null | grep -m1 "session id" | awk '{print $3}')
[ -z "$S" ] && { echo "no session id in $J"; exit 2; }
for attempt in 1 2 3 4 5 6 7 8; do
  [ -f $J/codex.log ] && mv $J/codex.log $J/codex-attempt-$(date +%H%M%S).log
  cd $J && codex exec --skip-git-repo-check -m gpt-6.1-sol -c model_reasoning_effort=medium -c sandbox_mode="danger-full-access" -C $W resume $S \
    "앞 세션이 모델 용량 오류로 끊겼다. git status와 로그 폴더부터 확인해 이어서, brief($J/brief.md)의 남은 일을 끝낸다. 이미 끝난 단계는 다시 하지 않는다." < /dev/null > $J/codex.log 2>&1
  rc=$?; echo "exit=$rc" >> $J/codex.log
  if [ $rc -ne 0 ] && grep -q "at capacity" $J/codex.log; then sleep $((attempt*180)); continue; fi
  exit $rc
done
