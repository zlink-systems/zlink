#!/bin/bash
# usage: relaunch.sh <jobdir> <worktree>   (blocking; codex stdin closed)
J=$1; W=$2
mv $J/codex.log $J/codex-stalled-stdin.log 2>/dev/null
cd $J && codex exec --skip-git-repo-check -m gpt-6.1-sol -c model_reasoning_effort=medium -c sandbox_mode="danger-full-access" -C $W "$(cat brief.md)" < /dev/null > codex.log 2>&1
echo "exit=$?" >> $J/codex.log
