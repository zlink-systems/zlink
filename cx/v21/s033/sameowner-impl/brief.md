Model gpt-6.1-sol, effort high: implementation of the APPROVED design. Earlier job briefs are history, not instructions.

# Task: implement C++ same-owner independent claim (#1639), approved design

Worktree /root/worktree/zlink-v21-s033/sameowner-impl, branch perf/1639-cpp-same-owner (= trole2 receive-role handoff PR branch perf/1639-cpp-receive-role-handoff, main already merged). Do not push or open a PR.
Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; read the copies already on disk.

## Approved design
/root/jobs/fx/cx/v21/s033/sameowner2/report.md — the PM approved "설계안 — 파일:줄별 변경" 1–5 exactly as written (2026-11-11 04:11Z). Line numbers there are at 76a4d78506; re-locate them on this branch.
- service_mailbox_t is the single owner. Owner property (independent: Node/Channel; serial: Spot/Actor/infrastructure) is decided at registration. No caller-side flags, no key-prefix guessing.
- Direct-claim permission sites 2 → 1, ready/drain decisions 7 → 1 (total 9 → 2). Add the immutable owner property and 2 cursors; remove the 6 counter definitions the design names. No new queue, lock, mutable flag, timer or thread.
- Independent claim takes exactly 1 record, moves it out under the mutex, publishes the next ready, then enters the handler. Serial owners keep today's budget/draining/retained lifetime.
- Keep W5 drain budget, role credits, receive-role handoff (offload_executor.cpp is NOT in scope) and the ready-callback "remove only after accepted" rule from trole2.

## Do
1. Product commit implementing 1–5. Apply the uncommitted repro test /root/jobs/fx/cx/v21/s033/sameowner2/repro2.diff (git apply) and turn it into regular tests: same Channel B enters before A is released (20/20), different Channel control, gated Actor stays 0/20 with both replies. Use the test lease settings the report describes (fixed RID readiness).
2. Enable trole2's T05 same-owner case (test_cpp_framework_receive_role_handoff.cpp, DISABLED_T05KnownFailureSameNodeOwnerProgress) — it must now pass.
3. Add the tests from "시험안": Node/Channel pre-existing backlog, FIFO take with reverse entry, B→A reverse and duplicate settlement, enqueue/cut/rollback/shutdown races, different-gate positive control, SpotWide/PerActor/Yield serial controls. Each new test needs a negative control that detects lost ready, duplicate publication, and a per-record executor wrapper; record each control's detection in the report.
4. Fail-before: run the new tests against the product files reverted to the branch base (perf/1639-cpp-receive-role-handoff) and record which fail.
5. Build with /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release preset linux-ninja-release, tests ON, binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON; never /root/.cache/zlink/zlink-cpp/<ver>), Core release prefix /root/.cache/zlink/core-release/1.24.0/linux-x64, -j3, -k 0, 25 GB cap, build dir inside the worktree. New tests 50x each, full C++ framework unit+contract suite once, format.sh --check cpp. Commit locally. Keep the build dir and write its path in the report.
6. No benchmarks; the supervisor measures.

## Rules
- No GitHub, no sub-agents or other AI tools. Keep every raw log in /root/jobs/fx/cx/v21/s033/sameowner-impl. Every claim file:line.
- Report in Korean, short: diff summary, actual decision-site and counter counts before/after, each test with pass counts and its negative control, fail-before results, known failures, risks, and one line "리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목, 넘긴 항목".

## Output: /root/jobs/fx/cx/v21/s033/sameowner-impl/report.md
