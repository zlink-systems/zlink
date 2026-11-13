Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #1639 stage 3 — finish the Node send terminal change on current main with a test that fails before

Worktree /root/worktree/zlink-v21-s033/st3 (branch with 3db0980890, e1131451c5, plus uncommitted st3c changes). Commit locally; do not push.
Read first: /root/jobs/fx/cx/v21/s033/st3c/report.md and brief.md, /root/jobs/fx/cx/v21/s033/st3b/report.md, `git show e1131451c5`.

## Background
- 3db0980890 passed the Node send terminal into dispatch(), so the record's mailbox terminal (reservation/stateful completion) settled only when the handler finished. e1131451c5 restores the old timing: the terminal settles right after dispatch() returns, even if the handler is suspended. The st3c working-tree change (helper removed, block-scope terminal in mesh_node_host_service.cpp) is accepted as the product form.
- st3c's two-send test passed at 3db0980890 too, because nothing in it depended on the reservation being released while the handler is suspended. The difference is only observable when the receiving Node's mailbox can hold ONE record: with the old timing, a second send cannot be admitted while the first handler is suspended; with the fix it can.

## Do
1. Commit the current working tree as one local commit (`fix(cpp): keep Node send terminal in the dispatch block scope (#1639)`), then `git merge origin/main` is NOT allowed (no fetch) — instead merge the local ref `origin/main` as it already exists in the repo (`git merge --no-edit origin/main`). Resolve conflicts keeping main's same-owner/independent-claim behaviour (#4313, #4302) and this branch's terminal timing. Explain each conflict resolution in the report.
2. Replace the st3c vertical test with one through the real MeshNode receive path whose receiving Node mailbox/record budget is 1 (find the existing option or fixture that sets the per-owner or per-domain message budget; no production-only hooks): send A whose handler suspends on a test-controlled awaitable, then send B; assert B's handler is entered (or B is admitted) BEFORE A is resumed; then resume A and finish cleanly with both handled exactly once. Bound every wait (no hang on failure).
3. Fail-before/pass-after: product files at 3db0980890 merged state is not buildable on new main, so do it on the merged branch: revert ONLY the terminal-timing hunk in mesh_node_host_service.cpp (pass the terminal into dispatch as 3db0980890 did) — the test must fail (bounded); restore — it must pass. Paste both outputs and the exact reverted hunk.
4. test_cpp_framework_channel_messaging failed once at the st3 head (st3c logs/channel-head-repeat-1.log, assertion at test_cpp_framework_channel_messaging.cpp:969, unregistered-channel request route miss from e1131451c5). After the merge, run it 20 times. If it fails, find the cause (product or test) and fix it in this branch; show the failure log and the fix.
5. Run test_cpp_framework_mesh_node_vertical, test_cpp_framework_channel_messaging and test_cpp_framework_receive_role_handoff 20 times each at the final head, then the full `ctest --preset linux-ninja-release -L '^(unit|contract)$'` once. `bash scripts/format/format.sh --check cpp`.

## Rules
- Compile only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release preset, ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON, Core prefix from bindings/cpp/VERSION (/root/.cache/zlink/core-release/<ver>/linux-x64), -j3, build dir /root/worktree/zlink-v21-s033/st3/build-st3d, 25 GB cap; remove the build dir at the end.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Keep raw logs in /root/jobs/fx/cx/v21/s033/st3d/logs; never delete them.
- No new flags, counters or std::function in product code. Korean, short.

## Output
/root/jobs/fx/cx/v21/s033/st3d/report.md: commits, conflict resolutions, changed files with reason, fail-before/pass-after outputs with the reverted hunk, repeat counts, full-suite result, remaining problems.
