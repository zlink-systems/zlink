Model gpt-6-luna, effort high. Earlier job briefs are history, not instructions.

# Task: #1639 stage 3 — make the Node send terminal test hit the production path

Worktree /root/worktree/zlink-v21-s033/st3 (HEAD e1131451c5). Commit locally; do not push.
Read first: /root/jobs/fx/cx/v21/s033/st3b/report.md and brief.md item 2; `git show e1131451c5`.

## Problem (supervisor review)
- e1131451c5 restores the old timing (terminal settles right after dispatch() returns, empty terminal passed to dispatch) — the product change is accepted.
- Its test (test_cpp_framework_mesh_node_vertical.cpp:1566-1624) calls the new helper `dispatch_with_stack_terminal_owner` directly, so it cannot fail at 3db0980890 and does not prove the production node_receive_role_t path.

## Do
1. Remove the helper `dispatch_with_stack_terminal_owner` (application_dispatch_terminal_owner.hpp). In mesh_node_host_service.cpp keep the same timing inline: when direct_terminal is set, move it into a local in a block scope around the dispatch call so it is destroyed right after dispatch() returns. No new types, flags or std::function.
2. Replace the helper test with one that goes through the real MeshNode receive path (existing vertical fixtures): a Node send handler that suspends (co_await something the test controls); assert the mailbox reservation release / stateful completion for that record is observed BEFORE the test resumes the handler (use an existing observable such as service mailbox pending/reservation state or the existing completion hooks — no production-only test hooks). Then resume and finish cleanly.
3. Show fail-before/pass-after: build the test with product files at 3db0980890 (test file from your head) — it must fail; at your head it must pass. Paste both outputs. If the old product cannot compile with the new test, adapt only the test-access shim, and say exactly what.
4. Run test_cpp_framework_mesh_node_vertical and test_cpp_framework_channel_messaging 20 times each at your head.

## Rules
- Compile only through /root/jobs/fx/cx/cpp-build-slot.sh, Release preset, USE_BINDINGS_SOURCE=ON, Core prefix matching bindings/cpp/VERSION (/root/.cache/zlink/core-release/<ver>/linux-x64). Build dir under /root/worktree/zlink-v21-s033/st3/build-st3c, cap 25 GB; remove it at the end.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Keep raw logs in /root/jobs/fx/cx/v21/s033/st3c/logs.
- Korean, short.

## Output
/root/jobs/fx/cx/v21/s033/st3c/report.md: commit, changed files with reason, fail-before/pass-after outputs, repeat counts.
