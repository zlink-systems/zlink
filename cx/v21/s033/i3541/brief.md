Model gpt-6-luna, effort high: Core fix with test. Earlier job briefs are history, not instructions.

# Task: #3541 Core connect session and connecter choose the io thread twice

Worktree /root/worktree/zlink-v21-s033/i3541 (detached at origin/main; create branch fix/3541-core-io-thread-choice). Issue text: `gh` is not allowed — read it here: connect-side socket picks the session io thread with choose_io_thread_transport (core/src/runtime/sockets/common/socket_base_endpoint.cpp:533 on main at the time) and session_base::start_connecting (core/src/runtime/core/session_base.cpp:854) picks again for the connecter with the same round-robin selector, so with io_threads=4 and 100 sequential TCP SUB connects sessions land 50/0/50/0 (TLS 45/6/44/5). Re-verify the lines at HEAD.

## Do
1. Decision-owner table: who decides the io thread for a connect (file:line), before/after; the fix must leave one choice per connect (the connecter uses the session's io thread, or whatever single owner the Core spec under core/doc/spec names — quote it). No new option/flag.
2. Core unit/regression test: io_threads=4, 100 (or 8) sequential connects → per-io-thread session counts balanced (max−min ≤ 1 for round-robin), TCP and TLS if the test infra has TLS. Negative control: the test fails on the unfixed code (run it before the fix, keep the log).
3. Build Core and run the new test 50×, the Core unit suite once, and `bash scripts/format/format.sh --check` for the touched language. Commit locally.

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- C++/Core compiles only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), -j3, 25 GB cap, build dir inside your worktree; Framework C++ builds use the binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON), never /root/.cache/zlink/zlink-cpp/<ver>.
- Any benchmark/measurement runs under `flock -o /root/jobs/fx/cx/measure.lock` for the measurement only; build/analysis never holds it. Per-thread sums from before/after snapshots use the TID union (template /root/jobs/fx/cx/v21/s033/pool2/analyze-22k.py). Keep every raw file; never delete raw outputs.
- Every claim file:line or raw-file path. Report in Korean, short. Remove build dirs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/i3541/report.md
