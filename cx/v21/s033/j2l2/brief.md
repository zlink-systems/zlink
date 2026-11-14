Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: Java ladder L2 (request+reply channel envelope header) — find what Java pays per message that .NET does not, and fix it in production Java Framework code.

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh or any git fetch/pull, never delete raw files. Do NOT take /root/jobs/fx/cx/measure.lock (profiles only; the supervisor measures). Do not touch other worktrees or their build outputs (read other code with `git show <commit>:<path>` only).

## Numbers
Supervisor quick compare 2026-11-14 00:50–00:53 UTC, branch exp/4350-java-fixes 52c1761ab0 (= main 6350275fac + #4414 13fdfe8c3b + #4419 typed JSON + Java ladder bench), timing warmup 1 s / active 5 s / 3 s gap / no warm run, one run per step:
| cell | L1 msg/s | L2 msg/s | loss |
|---|---:|---:|---:|
| send 1K | 431,734 | 389,098 | 9.9% |
| send 4K | 175,292 | 162,246 | 7.4% |
| serial 1K | 11,190 | 10,257 | 8.3% |
| serial 4K | 10,706 | 9,830 | 8.2% |
| backpressure 1K | 124,571 | 96,060 | **22.9%** |
| backpressure 4K | 87,729 | 79,264 | 9.6% |
Raw: /root/jobs/fx/cx/v21/s033/lfx-java/raw/q1114/index.tsv. Budget 7% per step.
Earlier (main, 10 s warmup): lad-java-l2/report.md and lad-java-l2b/report.md showed 5/6 cells within 7%. So part of the gap may be JIT warm-up of the L2 code within the 1 s window. Measure both (step 2) — the 1 s timing is the binding perf timing set by 대표님 and stays; a cost that only shows up cold is still a cost to remove (e.g. code that is too large or megamorphic to compile quickly).
Prior Java L2 analysis to read first: /root/jobs/fx/cx/v21/s033/l2bp/report.md (#4414: native envelope string scratch reuse, `ZLinkChannelEnvelope.java:1324-1327`), lad-java-l2e/report.md (production reply path `ZLinkChannelDispatchReporter.java:378-397`), and the Node vs .NET L2 table /root/jobs/fx/cx/v21/s033/n2net/report.md for the layout to copy.
.NET is the reference implementation (PM 11-13 21:35): a per-message cost .NET does not pay is not "contract-required". .NET L2 now passes all 6 cells (dser + lfx-dotnet). .NET files: framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs, ZLinkClientCallCodec.cs, ZLinkChannelReplyWriter.cs.

## Worktree (already created)
/root/worktree/zlink-v21-s033/j2l2, branch perf/4352-java-l2-header = origin/perf/4352-l2-bp-reply (#4414: 13fdfe8c3b + 2125999e62). Java Framework is under framework/languages/java. Build with the repo's gradle wrapper (`./gradlew --no-daemon --max-workers=2 ...`), pinned `taskset -c 6-7,14-15`. Java test runs go through `/root/jobs/fx/cx/test-slot.sh 8-15 <command>` (it takes the shared measure lock — keep each test run short and do not hold it while idle).
Profiling scratch: copy the Java ladder bench from exp/4350-java-ladder f3e3933567 (`git archive f3e3933567 framework/bench/grpc/java | tar -x -C /root/worktree/zlink-v21-s033/j2l2-scratch`), build it against THIS worktree's Framework (composite build as lfx-java did — see /root/jobs/fx/cx/v21/s033/lfx-java/progress.md and tools/run-one.sh for the launch commands and ZLINK_BENCH_LADDER_L1 / _L2 switches). Record jar SHA256s.

## Steps
1. Read the prior reports above. List every per-message L2 operation on: client request header encode, target header parse, target reply header encode, client reply header parse + correlation completion. Write the planned table skeleton into /root/jobs/fx/cx/v21/s033/j2l2/progress.md.
2. Profile L1 vs L2 for request-backpressure 1K and send-saturation 1K, source and target, with JFR (allocation + CPU sampling) or async-profiler if installed; every process pinned `taskset -c 6-7,14-15`; two windows per run: the first 2 s after start (cold) and a 5 s window after 15 s (steady); each process ≤ 30 s with a hard timeout. Per message, with file:line: objects/bytes allocated by L2 code, Strings created, byte[]/char[] copies, maps/collections touched, exceptions, and JIT state of the top L2 methods (`-XX:+PrintCompilation` or JFR compilation events: is the method still interpreted/C1 at 1–2 s?). Check errors 0 and submitted = completed = received.
3. Table Java vs .NET per cost item with file:line (both sides). Mark what Java pays and .NET does not.
4. Fix in production Java Framework code (zlink-framework-core runtime/messaging and runtime/channels), biggest item first, stopping when the remaining L2 per-message cost is at .NET's shape. Rules: wire bytes identical; one writer/parser per header (no fast path + fallback); rule count (decision locations: who decides header bytes, who owns buffers, who validates) must not grow — table before/after; no new flag/queue/lock/cache/timer/ThreadLocal. If a fix needs a binding API, do not change the binding; write the exact API needed.
5. Tests in the existing Java test tree for the touched classes: byte-identical request/reply/command headers vs current output (deadline present/absent, flow on/off, metadata, escapes, 1- and 64-byte correlation), decoded values equal, and an allocation regression (ThreadMXBean allocated bytes per header encode/parse) that fails before / passes after. Run those tests plus `zlink-framework-core:test` for the touched packages, and `bash scripts/format/format.sh --check java`.
6. Re-profile the same cells after the fix (cold + steady) and put before/after per-message numbers side by side.
7. Local commit(s) only, Korean message like recent repo perf commits, ending with (#4352).

## Output
/root/jobs/fx/cx/v21/s033/j2l2/report.md (Korean, short): per-message cost table Java vs .NET with file:line, cold vs steady findings (JIT), what changed and why, rule-count table, tests with fail-before/pass-after log paths, before/after profile table, commit hash. Raw under /root/jobs/fx/cx/v21/s033/j2l2/raw. Time cap 2.5 h.
