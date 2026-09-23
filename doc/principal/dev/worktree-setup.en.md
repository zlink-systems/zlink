# Worktree setup — set up the machine once, then `rebuild-dev → gate`

This document owns the procedure and rationale for preparing a worktree to run the gate. The
issue and PR workflow is documented in [`development-workflow.ko.md`](./development-workflow.ko.md).

## 3. Create a worktree — local packages are linked

`scripts/dev/work.sh start --issue <N>` creates the branch and worktree at
`/home/hep7/worktree/zlink-<N>-<slug>` in WSL. The Windows `scripts/dev/work.ps1` uses
`D:\worktree\zlink-<N>-<slug>`. Both prepare local packages; binding packages are linked rather
than rebuilt when the shared cache can be used. Use `--no-packages` when packages are unnecessary.

## 6. Cleanup

`work.sh done --verified <sha>` (Windows `work.ps1`) verifies the PR head SHA, merges an open PR
or continues cleanup for an already merged PR, then removes the remote branch and worktree. For a
`Closes #N` PR it deletes the local branch, closes the issue with a comment linking the PR if GitHub
did not close it automatically, deletes WSL verification copies matching
`/home/hep7/worktree/zlink-<N>*`, and updates the Project to Done. Already completed removals and
issue closure are skipped. The package cache remains.

Before removing a worktree, check that no job is still running there.
