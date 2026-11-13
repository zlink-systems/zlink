Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: Java L4 typed payload (default JSON serializer) loss — first check the bench DTO and the wire contract against the spec, then find the cause and fix the production encode path. Report first (report.md), then implement.

Worktree /root/worktree/zlink-v21-s033/j4json, branch perf/4354-java-typed-json, from origin/main df42a4df40. Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Never delete raw files. Do NOT take /root/jobs/fx/cx/measure.lock. Other jobs are MEASURING: never touch worktrees lad-java, lad-java-l57p, lad-node, l3868-next, lad-dotnet or their build outputs; read their code with git show only. Profiling (PM 11-13 19:46: profile without the lock): pin every process you start to CPUs 6-7,14-15 with taskset, each profiled run at most 20 s; compare shapes, not throughput.
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md, ladder-steps.md (L4 serializer table), issues-4351-4357.md (#4354), lad-java-impl4/report.md, lad-java-s2/report.md (numbers). Ladder code: exp/4350-java-ladder 9d9901eb2c (RawStack.java ~:481, ZLinkFrameworkBenchServer.java ~:348 use ZLinkJsonMessageSerializer; DTO BenchTypedPayload is a record with one byte[] body).

## Evidence (lad-java-s2, 0-5/8-13 pinned)
L4 vs L3 loss: send-saturation 1K 52.0%, 4K 61.9%; request-serial 1K 10.5%, 4K 21.4%; request-backpressure 1K 25.6%, 4K 56.9%. 4K worse than 1K points at per-byte work.

## Step 1 — spec checks (PM 11-13 20:16), report these FIRST, with file:line
a. Bench DTO shape: is a DTO with a single byte[] body (base64 JSON) the payload the performance spec defines for the typed step (#4354, the perf schema / benchmark payload definition in the spec and bench contract docs)? Quote the spec file:line that defines the bench payload, and the bench DTO file:line in each language (Java BenchTypedPayload; C++ ladder_payload.hpp in 276b84f248; Node ae8a5178b5; .NET 4cbe0b5316 — git show only). Do NOT change the DTO or the measurement conditions; if they differ from the spec, report it and stop there (the PM decides with 실장님).
b. Wire contract: is byte[] → base64 string the wire format the typed JSON contract (spec) defines? Quote the spec file:line. If the spec defines it, the wire stays byte-identical; only copies/allocations on the encode/decode path may change.

## Step 2 — cause
Profile L3 vs L4 send-saturation 4K and request-serial 1K (async-profiler or JFR: cpu + alloc) in a scratch copy you build under /root/worktree/zlink-v21-s033/j4json-scratch (from 9d9901eb2c; never build in the measuring worktrees). Count allocations and copies per message on the production ZLinkJsonMessageSerializer path with file:line (base64 via intermediate String/char[]/StringBuilder, tree model instead of streaming, per-call reflection or mapper/writer creation, buffer growth or copies into the zlink Message, UTF-8 re-encoding — check, do not assume).

## Step 3 — report, then fix
Write /root/jobs/fx/cx/v21/s033/j4json/report.md: step 1 answers (a, b) with file:line; cause with production file:line; profile evidence under j4json/raw; production fix proposal (owner layer, spec reference, rule count before/after). Stop after the report if the wire format, a spec or a public API would change, or if step 1a found a mismatch. Otherwise implement in production code (framework/languages/java) with unit tests that pin byte-identical JSON output and the removed copies/allocations where testable; run the Java format check and the framework tests covering the touched files. No bench-only fixes.
Time cap 4 h.
