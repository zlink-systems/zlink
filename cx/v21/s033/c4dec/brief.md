Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: C++ default typed JSON decode structure — compare with .NET Utf8JsonReader, choose by rule count and allocations/message, implement the winner in production.

Never push, no GitHub, no sub-agents or other AI tools, never run internal-docs.sh, never delete raw files. Do NOT take /root/jobs/fx/cx/measure.lock. Do not touch other worktrees or their build outputs (read other code with git show only).
Read first: /root/jobs/fx/cx/v21/s033/common-rules.md (all, incl. BUILDS BETWEEN HOLDS), /root/jobs/fx/cx/v21/s033/c4json/report.md (previous step: base64 path fixed in commit aae182694c on perf/4354-cpp-typed-json, now PR-bound), its raw/ allocation files.

## Worktree
`git -C /root/zlink worktree add /root/worktree/zlink-v21-s033/c4dec -b perf/4354-cpp-json-decode perf/4354-cpp-typed-json` (aae182694c). C++ builds only via /root/jobs/fx/cx/cpp-build-slot.sh (Release, USE_BINDINGS_SOURCE=ON, Core prefix 1.25.0 at /root/.cache/zlink/core/1.25.0/linux-x64, -j3, ≤25 GB, build dir inside this worktree). Pin every process you start to CPUs 6-7,14-15; profiled runs ≤20 s.

## PM position (11-13 23:12 UTC)
After c4json, 4K decode still costs 26 allocations / 36,306 B per message (1K: 22 / 9,424). These come from the JSON reader implementation (nlohmann lexer raw-token vector growth `lexer.hpp:1358` 12-14x, unescaped token strings `lexer.hpp:1408` 7-9x, parser state, SAX frame) — an implementation choice, not a wire contract. .NET reads the same JSON with Utf8JsonReader over the input span without a DOM. Do not label these "contract-required".

## Steps
1. Table, per decode cost item, C++ file:line (common/include/zlink/json_profile.hpp and nlohmann) vs .NET file:line (framework/languages/dotnet: ZLinkFrameworkJsonPayloadCodec.cs and the reader it uses): allocations, copies, validation done, for a DTO with a byte field at 1K and 4K and for the real ladder L4 DTOs (request/reply).
2. Candidates (at least): (a) keep nlohmann SAX but stop its token growth (reuse/reserve, string_view tokens where the API allows); (b) a direct span reader for the Framework JSON v1 profile (the profile's own grammar) replacing nlohmann for decode on this path. For each: decision locations (rule count) before/after — must be ≤ today's; identical accept/reject set and identical decoded values (reuse c4json's fixtures + the shared golden framework/runtime/protocol/golden/framework-json-v1.json + required-presence golden; add malformed/escape/unicode/duplicate-key/number-edge cases); allocations and bytes per decode at 1K/4K (same counter as c4json); CPU shape with perf.
3. Recommend one with the numbers, then implement it in production with tests that fail before / pass after on the allocation budget and pass on the full accept/reject set. Do not keep two decoders for the same profile (no fallback second path). C++ format check, framework tests covering touched files. Local commit only.

## Output
/root/jobs/fx/cx/v21/s033/c4dec/report.md (Korean, short): comparison table with file:line, candidate table (rule count, allocations/B per decode, accept/reject parity), choice and why, before/after, tests, commit hash. Time cap 2.5 h.
