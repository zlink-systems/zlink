Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: reduce copies and allocations on the production typed JSON byte[] <-> base64 encode/decode path (Java first), wire unchanged. Then compare the same path in C++, .NET and Node with file:line.

Worktree /root/worktree/zlink-v21-s033/j4json, branch perf/4354-java-typed-json, from origin/main df42a4df40 (clean). Commit locally only; never push, never open a PR, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh. Never delete raw files. Do NOT take /root/jobs/fx/cx/measure.lock. Other jobs are MEASURING: never touch worktrees lad-java, lad-java-l57p, lad-node, l3868-next, lad-dotnet or their build outputs; read their code with git show only. Profiling (PM 11-13 19:46): pin every process you start to CPUs 6-7,14-15 with taskset, each profiled run at most 20 s; compare shapes, not throughput.
Read first: /root/jobs/fx/cx/v21/s033/j4json/report.md (previous step: spec checks — base64 is the wire contract, 05-message-model.ko.md:153; keep it), common-rules.md, ladder-steps.md, lad-java-s2/report.md (numbers: L4 vs L3 loss send 52%/62%, serial 10%/21%, bp 26%/57%; 4K worse than 1K).

## PM decision 11-13 20:28
- Wire stays exactly as the spec defines (padded RFC 4648 base64). Fix only the production encode/decode path. This is a real-user cost, independent of the bench payload question (do NOT change the bench DTO or measurement conditions).
- All four languages share the same serializer contract: once the Java cause is found, compare the same path in the other three with file:line.

## Do
1. Profile the L3 vs L4 bench path (send-saturation 4K, request-serial 1K) with async-profiler or JFR (cpu + alloc) from a scratch build under /root/worktree/zlink-v21-s033/j4json-scratch (from exp/4350-java-ladder 9d9901eb2c; never build in the measuring worktrees). Count copies and allocations per message on the production ZLinkJsonMessageSerializer encode and decode paths for a byte[] property, with file:line (base64 via intermediate String/char[]/StringBuilder, tree model vs streaming, per-call reflection or writer creation, output buffer growth and copies into the zlink Message, UTF-8 re-encoding — check, do not assume).
2. Write /root/jobs/fx/cx/v21/s033/j4json2/report.md FIRST: cause with production file:line, profile evidence under j4json2/raw, fix proposal (owner layer, spec reference, rule count before/after). Stop there if a spec or public API change would be needed.
3. Implement in production Java (framework/languages/java), with unit tests that pin byte-identical JSON output for byte[] fields (sizes 0, 1, 2, 3, 127, 128, 1024, 4096) and the removed copies/allocations where testable. Run the Java format check and the framework tests covering the touched files.
4. Cross-language table in report.md: for C++ (typed JSON serializer, I/contracts/codecs/serializer.hpp and the JSON byte-sequence writer/reader), .NET (ZLinkFrameworkJsonPayloadCodec / System.Text.Json byte[] path) and Node (channel-envelope.ts default JSON codec, Buffer base64), list the same per-message copies/allocations with file:line and whether the Java cause exists there. Do not change those languages in this job; report only.
Time cap 4 h.
