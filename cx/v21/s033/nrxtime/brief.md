Model gpt-6-luna, effort high: the duplicate is already pinned to file:line by an earlier study; this is a small Node change plus tests and one same-session measurement.

# Task: Node receive timestamp made once, not twice (Refs #3872)

## Situation
The Node send study (/root/jobs/fx/cx/v21/s033/nsend/report.md, read the 중복 작업 점검 paragraph) found that the first-receive UTC time is created twice: once in the mailbox (framework/languages/node/packages/framework/src/runtime/foundation/service-mailbox.ts, around :113 in that study's snapshot) and again by the channel dispatcher fallback (channel-dispatchers.ts:818 `record.receivedAt ?? this.receivedAtClock()`), because the general record conversion (node-raw-mesh-backend.ts around :2674) drops the mailbox time. The second clock self cost was about 0.146 µs/msg. The existing receiveIngressLifecycle helper carries the time but also builds a release closure, so do not use it just for the time.
Rule from the spec: the message model's UTC receive time must be the first receive time; it must not shift with queue delay.
Worktree: /root/worktree/zlink-3872-rxtime = main a60d04f8cf + CPU-pin patch. Line numbers may have moved; re-locate them.

## Do
1. Trace where the receive time is created and every place it is read, for send, request and reply paths (file:line). Quote the spec line for the UTC receive time (rg in framework/doc/framework for receivedAt / 수신 시각).
2. Create the time once at the first receive point and carry it through the record conversion so the dispatcher uses it; keep the dispatcher fallback only if some path still has no time, and say which path.
3. Tests: a contract test that the time seen by the handler equals the mailbox time (not a later clock read) even when the record waits in the queue; run the full Node `npm test` and record the log.
4. Measure, binding in the same session: unpaced RUNS=5 request-serial 1KiB and send-saturation 1KiB, main vs change, warmup 10 s and active 20 s (the standard bench durations).

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- This task is only the receive timestamp. Do not read other job folders under /root/jobs/fx/cx/v21/s033 except nsend/report.md, and do not write outside nrxtime/ and this worktree. An earlier run of this brief drifted into a different job's brief; do not do that.
- No git operations except git diff and git status; no spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh.
  Do not modify bindings/ or core/. Changes only in framework/ and bench files of this worktree.
- Builds/tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: `flock -o /root/jobs/fx/cx/measure.lock` (exclusive; keep waiting if held), condition B (source CPUs 0-7, target 8-15).
- MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &.
- 리팩토링 점검: list duplicate work you see, with file:line, and whether your change removed it; no new queue, lock, cache, flag or state.
- Framework÷binding means Framework throughput ÷ binding throughput (below 1 when the Framework is slower). Report "over budget by X µs" per message (budget = binding µs / 0.6). Do not suggest lowering the 0.6 target.
- Run the Node format check (scripts/format/format.sh --check node) and git diff --check at the end.
- Time box about 3 hours; time waiting for the measurement lock does not count.

## Output: /root/jobs/fx/cx/v21/s033/nrxtime/report.md (Korean, plain sentences): (1) time creation/read sites and spec quote; (2) change and decision-site count before/after; (3) tests with log paths; (4) RUNS=5 table. Save framework/ diff as nrxtime/change.diff.
