Model gpt-6.1-sol, effort high: implementation of the APPROVED design. Earlier job briefs are history, not instructions.

# Task: implement the C++ target receive-role handoff experiment (#1639), design A

Worktree /root/worktree/zlink-v21-s033/trole, branch perf/1639-cpp-target-receive-role at 76a4d78506. Do not push or open a PR.
Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch; read the copies already on disk.

## Approved design
/root/jobs/fx/cx/v21/s033/trole/design.md — the PM approved §6 A exactly as written (2026-11-11): executor becomes the single owner of receive-role worker selection (decision sites 12 → 11, module owner 1 → 1); host run() does one rotation and returns; after the rotation and publication stacks return, one try_lock, assign the role to an already-idle worker; the former receiver takes the normal queue head through the existing claim/gate/permit path; on any failure keep today's path. Replace `_roles` and the `_idle_workers` count with per-worker assignment slots and an idle list — replace, do not add. No new flag, counter, timer, lock, thread or start-confirmation wait.

## Do
1. Implement §6 A in the product commit only. Keep §6 "종료·예외" rules.
2. Tests T01–T11 from design §8 with their negative controls (T12 cross-language parity is later). T05's same-owner condition is a KNOWN PRE-EXISTING FAILURE (service_mailbox.cpp:212-217, 235-237; being handled separately): write it, mark it as an expected/known failure in the test the way the repo marks known failures (or keep it out of the pass set and say so), and judge only the other T05 conditions.
3. A separate measurement-only commit with env-gated counters (attempts A, successes H, eligible rotations E, rotations R, records started locally) — like /root/worktree/zlink-v21-s033/pool commit 17c5a80757; never in the product commit.
4. Build with /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release, tests ON, binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON; never /root/.cache/zlink/zlink-cpp/<ver>), -j3, -k 0, 25 GB cap, build dir inside the worktree. New tests 50× each, full C++ framework unit+contract suite once, format.sh --check cpp. Commit locally. Keep the build dir (the supervisor measures from it) and write its path in the report.
5. No benchmarks; the supervisor runs §9.

## Rules
- No GitHub, no sub-agents or other AI tools. Every claim file:line. Report in Korean, short: diff summary, actual owner/rule counts after the change, each test with pass counts and its negative control, known failures, risks.

## Output: /root/jobs/fx/cx/v21/s033/trole2/report.md
