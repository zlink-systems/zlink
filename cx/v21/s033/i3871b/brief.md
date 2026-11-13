Model gpt-6-luna, effort high: evidence work — per-stage wall-time instrumentation of one Java request-serial cell plus a four-language decision-owner table (read-only on product code; the PM designs the change). Earlier job briefs are history, not instructions.

# Task: #3871 — where the Java request-serial residual ~24 µs goes, and who runs completion in each language

Folder JOB=/root/jobs/fx/cx/v21/s033/i3871b. Worktree /root/worktree/zlink-v21-s033/i3871b (detached at origin/main). Read first: /root/jobs/fx/cx/v21/s033/i3871/report.md (previous diagnosis: Framework RTT +46 µs over binding; target stamp→handler +23 µs; residual +24 µs not separable with the 1 ms flow log; source ForkJoinPool.commonPool worker 13 µs CPU/req and +3 context switches/req, posted at ZLinkServiceCompletionDispatcher.java:51 from ZLinkServiceOperationRegistry.java:407).

## Do
1. Per-stage wall time for Java Framework request-serial 1024 (and binding for the matching stages), using temporary nanoTime stamps (System.nanoTime, stored into preallocated arrays, dumped at exit — no logging on the hot path). Stages on the source: caller submit → native send returns; reply record received by the receive owner → terminal claim (ZLinkServiceOperationRegistry.java:407) → completion posted → commonPool worker starts the item → future completed → caller continuation resumes. On the target: handler return → reply submitted. Report p50/mean per stage for 3 rounds, condition B (source CPUs 0–7, target 8–15), same session, under `flock -o /root/jobs/fx/cx/measure.lock` for the measurement only. Also one run without the stamps to show they cost < 2 µs of RTT. Revert all instrumentation afterwards (git status clean) and keep the patch as JOB/stamps.patch.
2. Decision-owner table across C++, .NET, Java, Node: the decision "after a request reaches terminal state, which execution resource runs the caller's completion, and through what post"; for each language, every place that decides it (file:line), what it posts to (pool / dedicated dispatcher / inline / event loop), and the clause that owns it (S08 §4.4 in framework/doc/framework/common/spec/server/01-execution/08-messaging-hot-path.ko.md, S01 §11 in 01-submit-and-completion.ko.md, plus each language's own doc if it names the resource). The key question: is Java the only language that posts every completion to a shared pool, and how do C++ (completion_dispatcher_t, 2 workers since #4179), .NET and Node do it?
3. No product design and no product change: the PM designs from your table.
4. Report sections: 결과(단계별 표), 결정 소유 표(네 언어 file:line·조항), 검증(명령·로그 경로·원본 경로), 리팩토링 점검 한 줄("리팩토링 점검: 성능 N건, POSDDD N건, 불필요 코드 N건 발견. 고친 항목 0, 넘긴 항목"), 남은 문제. Label all numbers "옛 조건(제출 thread 하나)".

## Common rules
- Do NOT run scripts/dev/internal-docs.sh or any git pull/fetch/push; no GitHub, no sub-agents or other AI tools.
- Java build uses the Core 1.24.0 release package: P=/root/.cache/zlink/core-release/1.24.0/linux-x64, ZLINK_CORE_SOURCE=release ZLINK_CORE_PACKAGE_PREFIX=$P. Reuse the previous job's bench build/run commands (see /root/jobs/fx/cx/v21/s033/i3871/report.md and its codex log /root/zlink/.artifacts/codex/i3871/job.log).
- Per-thread sums from before/after snapshots use the TID union. Keep every raw file in JOB; never delete raw outputs.
- Every claim file:line or raw-file path. Report in Korean, short. Remove build outputs at the end; leave the worktree.

## Output: /root/jobs/fx/cx/v21/s033/i3871b/report.md
