Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: .NET ladder L2 (request+reply channel envelope header) loss — cause with file:line, C++ comparison, production fix.

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files. Do NOT take /root/jobs/fx/cx/measure.lock (profiles only; the supervisor measures). Do not touch other worktrees or their build outputs (read other code with git show only).
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (all, incl. BUILDS BETWEEN HOLDS), ladder-steps.md, lad-dotnet-s1/report.md (step table), n2net/report.md (it already tabulates the .NET L2 header path with file:line — start from it).

## Numbers (lad-dotnet-s1, L1→L2 loss, budget 7%)
send-saturation 1K +2.7%, 4K +4.4%; request-serial 1K +6.7%, 4K **+22.7%**; request-backpressure 1K **+12.5%**, 4K **+9.6%**. Request/reply paths fail, one-way send passes → reply-side cost (reply header encode on target, reply header decode + completion on source). serial 4K 22.7% vs serial 1K 6.7% is odd for a header-only step: check first whether it is a body copy or a reply buffer sizing issue that scales with payload, or a measurement artifact (look at lad-dotnet-s1 raw for that cell: errors, submitted/completed, run-to-run).

## Worktree
`git -C /root/zlink worktree add /root/worktree/zlink-v21-s033/d2l2 -b perf/4352-dotnet-l2-envelope origin/main`. Scratch ladder: git archive of the .NET ladder at 69891f4446 (L2) into /root/worktree/zlink-v21-s033/d2l2-scratch, using this worktree's production .NET. Pin every process you start to CPUs 6-7,14-15; profiled runs ≤20 s; compare shapes, not throughput. .NET builds: Release, the same local package roots lad-dotnet used.

## Steps
1. dotnet-trace (cpu-sampling) + allocation counts (dotnet-counters / EventPipe GC AllocationTick or MemoryDiagnoser-style probe) of L1 vs L2 for request-serial 4K and request-backpressure 1K, source and target. Count allocations, copies, validations per message on the envelope/reply path with file:line (ZLinkEnvelopeCodec.cs, ZLinkClientCallCodec.cs, ZLinkChannelReplyWriter.cs, ZLinkClientServerDispatcher.cs and the completion path).
2. Table .NET vs C++ (framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp, client_call_codec.cpp) per cost item, and say where .NET pays something C++ does not.
3. Fix in production .NET. Rule count (decision locations) must not grow; no new flag/queue/lock/cache. Wire bytes identical: tests pinning request/reply header bytes and decoded values, plus an allocation regression that fails before / passes after. `scripts/format/format.sh --check dotnet` and the framework tests covering touched files. Local commit only.

## Output
/root/jobs/fx/cx/v21/s033/d2l2/report.md (Korean, short): the serial-4K finding, profile shape L1 vs L2, .NET vs C++ table with file:line, fix and why, rule count before/after, allocations/copies per message before/after, tests, commit hash. Time cap 2.5 h.
