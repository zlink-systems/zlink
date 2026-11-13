#!/bin/bash
cd /root/zlink
until [ "$(gh pr view 4289 --json state -q .state 2>/dev/null)" = MERGED ]; do sleep 120; done
for p in 4266 4251 4231; do s=$(gh pr view $p --json state -q .state); echo "$p $s"; [ "$s" = OPEN ] && gh pr edit $p --add-label reviewed,ready-for-queue,merge-queue >/dev/null && gh pr comment $p --body "@mergifyio queue" >/dev/null && echo "$p requeued"; done
echo "4289 merged $(date -u +%T)"
