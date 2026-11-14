Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: .NET ladder L3 (service wire multipart framing) — cause of the per-message loss with file:line, compared with C++/Java/Node. DIAGNOSIS + PROPOSED FIX ONLY: do not change production code (the PM reviews .NET design first). You may write throw-away experiments in the scratch tree to prove a cause.

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh or any git fetch/pull, never delete raw files. Do NOT take /root/jobs/fx/cx/measure.lock (profiles only; the supervisor measures). Never `kill -CONT` processes the guard stopped (state T) — wait instead. Do not touch other worktrees or their build outputs (read other code with `git show <commit>:<path>` only).

## Numbers (supervisor, 2026-11-14 00:58–01:02 UTC, branch exp/4350-dotnet-fixes 4bd6102170 = main + #4418 first version + .NET ladder bench; warmup 1 s, active 5 s, 3 s gap, no warm run; guard active)
| cell | L2 msg/s | L3 msg/s | loss |
|---|---:|---:|---:|
| send 1K | 373,658 | 303,213 (first hold 295,954) | **18.9%** |
| send 4K | 149,977 | 142,660 | 4.9% |
| serial 1K | 7,899 | 7,786 | 1.4% |
| serial 4K | 7,576 | 7,399 | 2.3% |
| backpressure 1K | 62,963 | 61,083 | 3.0% |
| backpressure 4K | 3 pairs 11.2 / 3.9 / 9.0% | median 73,073 → 66,485 | **9.0%** |
Raw: /root/jobs/fx/cx/v21/s033/dl3/results.txt and raw/. Budget 7%. Send 1K losing much more than send 4K points to a per-message (not per-byte) cost on the one-way path; check which side (source or target) saturates in send 1K.
Step definition (L3 adds service wire multipart frame pack on send/reply and unpack on receive/completion): /root/jobs/fx/cx/v21/s033/ladder-steps.md. .NET ladder bench: exp/4350-dotnet-ladder 29d9fca82b (L3 switch: ZLINK_DOTNET_LADDER_LEVEL=L3); runner tools /root/jobs/fx/cx/v21/s033/lfx-dotnet/tools/dotnet/run-one.sh.
Related prior work to read: /root/jobs/fx/cx/v21/s033/n3sw/report.md (Node L3: final frame written into native storage; PR #4415 commits afd3a0cea4, ec6aa8ea5c), /root/jobs/fx/cx/v21/s033/d2l2/report.md (.NET L2 layout of a cost table), /root/jobs/fx/cx/v21/s033/lad-dotnet-s1 or lfx-dotnet/call-shape.md (L3 call path in the bench).

## Worktree
/root/worktree/zlink-v21-s033/d3l3, branch perf/4353-dotnet-l3-wire = origin/main 6350275fac (clean; leave production code unchanged). Scratch: `git archive 29d9fca82b framework/bench/grpc/dotnet | tar -x -C /root/worktree/zlink-v21-s033/d3l3-scratch` linked to this worktree's Release .NET Framework build (same way d2l2 did — see /root/jobs/fx/cx/v21/s033/d2l2/report.md "profile" section and raw/before-built-manifest.json). Build pinned `taskset -c 6-7,14-15`, Release.

## Steps
1. Find the production .NET service wire frame pack/unpack used at L3 (search framework/languages/dotnet/src for the multipart application frame encode/decode used by the mesh send/reply paths; the bench call site is in the ladder ZLinkServer/Program.cs and Client/Program.cs at 29d9fca82b). Write the call path, source and target, into /root/jobs/fx/cx/v21/s033/d3l3/call-shape.md with file:line.
2. Profile L2 vs L3 for send-saturation 1K and request-backpressure 4K, source and target processes, all processes pinned `taskset -c 6-7,14-15`, each ≤ 20 s with a hard timeout: `dotnet-trace` (cpu-sampling + gc-verbose) or `DOTNET_gcServer=0` + EventPipe allocation tick. Per message with file:line: managed allocations (objects, bytes), byte copies (count, size: which buffer → which), native Message creations, parts arrays/wrappers, length prefix writes, validation passes. Check errors 0 and submitted = completed = received. Also note per-thread CPU (which thread saturates).
3. Table .NET vs C++ vs Java vs Node for each L3 cost item (frame pack, part count/length encode, copy into native message, receive-side unpack views vs copies, reply pack, completion unpack), file:line on every side (C++ framework/languages/cpp/framework/src, Java framework/languages/java/zlink-framework-core/src/main/java, Node framework/languages/node/packages/framework/src including #4415 via `git show ec6aa8ea5c:<path>`). Mark what .NET pays and another language does not.
4. Proposed fix: concrete change list (files, functions), expected per-message effect (allocations/copies before → after; you may prove it with a scratch experiment and an allocation probe), rule-count table (decision locations: who decides frame bytes, who owns buffers, who closes them) that must not grow, wire bytes identical, tests that would pin it (byte-identical frames, ownership, allocation regression). No new flag/queue/lock/cache/timer.

## Output
/root/jobs/fx/cx/v21/s033/d3l3/report.md (Korean, short): which side/thread saturates in send 1K, per-message cost table L2 vs L3 with file:line, cross-language table, cause, proposed fix with expected effect and rule count, scratch experiment results if any (marked as experiment, not production). Raw under /root/jobs/fx/cx/v21/s033/d3l3/raw. Time cap 2 h.
