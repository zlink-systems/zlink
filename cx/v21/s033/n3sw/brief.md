Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: find and fix the cause of the Node L3 service wire framing loss. Report cause + production fix proposal first (report.md), then implement the production fix on this branch.

Worktree /root/worktree/zlink-v21-s033/n3sw, branch perf/4353-node-service-wire, from origin/main df42a4df40. Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Never delete raw files (yours or earlier jobs'). Do NOT take /root/jobs/fx/cx/measure.lock. Other jobs are MEASURING on this machine: never touch their worktrees (lad-java, lad-node, l3868-next, lad-dotnet) or build outputs; read their code with git show / git -C <worktree> show only. Profiling runs (PM 11-13 19:46: profile without the lock): pin every process you start to CPUs 6-7,14-15 with taskset, keep each profiled run ≤ 20 s, and treat the absolute numbers as noisy — compare shapes (where time and waits go), not throughput.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md, ladder-steps.md, issues-4351-4357.md (#4353), lad-node-l2f/report.md, lad-node-impl2/report.md (L3), and the numbers in lad-node-s1/progress.md. For comparison read how C++ (l3868-impl2/report.md, call-shape-L3.md: production retained-frame decoder, no extra copies) and Java (fea12ea97a) do the same framing.

## Evidence
Node L3 vs L2 throughput loss (1 run, 0-5/8-13 pinned): send-saturation 1K 30.8%, 4K 35.1%; request-serial 15.8%, 13.9%; request-backpressure 19.0%, 19.3%. L3 = service wire multipart framing (production pack/unpack). Ladder code: exp/4350-node-ladder 20598f57a2 (shared/raw-wire.js requireProductionServiceWire and callers). A 4K loss larger than 1K points at per-byte work (copies, Buffer.concat, base64/utf8, subarray→copy).

## Do
1. Reproduce from a scratch build (create a scratch worktree /root/worktree/zlink-v21-s033/n3sw-scratch at 20598f57a2's branch tip or check out the bench files; never build in the measuring worktree lad-node). Profile L2 vs L3 send-saturation 4K and request-serial 1K with --cpu-prof (and --heap-prof or allocation sampling). Count copies and allocations per message on the production service-wire encode/decode path, with file:line.
2. Write /root/jobs/fx/cx/v21/s033/n3sw/report.md FIRST: cause with production file:line (packages/framework/src/... service wire codec), evidence (profiles under n3sw/raw), production fix proposal (owner layer, spec reference such as 02-channel-transport/06-wire-protocol, rule count before/after). Stop there if a spec or public API change is needed; otherwise continue.
3. Implement the fix in production code framework/languages/node on this branch (commit(s) with unit tests that pin the behaviour: e.g. no extra copy, byte-identical wire output), run the Node format check and the framework tests covering the touched files. No bench-only fixes.
Time cap 4 h.
