Model gpt-6-luna, effort high: per-record object removal, no scheduling change. Earlier job briefs are history, not instructions.

# Task: #1639 design step 3 — resolve record dispatch at registration (C++ target)

Worktree /root/worktree/zlink-v21-s033/st3, branch perf/1639-cpp-record-dispatch-plan at origin/main 76a4d78506. Do not push or open a PR. Another job changes receive-role placement in parallel; do not touch dispatch/offload_executor.*, receive rotation, wake or worker policy.

## Scope
Approved design /root/jobs/fx/cx/v21/s033/cpath/report.md §7 row 3 and §4.2 R6–R9, §8 refactor table: build route/reply/codec/DI creation plans at registration and remove the per-record dispatcher object and per-record completion std::function (mesh/mesh_record_dispatcher.cpp:32·53·61·69 per p2/report.md §6 — re-verify lines at HEAD). Also read cplanr/report.md (recommends resolving hosted/standalone dispatch at registration and transferring the Node/Channel terminal directly instead of allocating a temporary shared terminal). Step 1 (#4155, canonical receive record) is merged; build on it. Keep every coroutine layer (design §4.4: never replace a coroutine with a callback). Fresh scope, handler and scoped dependency per record stay (API §10–11).

## Do
1. Decision-owner table before/after (file:line), rule count before/after, list of removed per-record objects with allocation counts before/after (static count; if the tree has an allocation-counting test harness, use it).
2. Implement. Tests: per-record dispatcher objects 0; internal type-erased closure heap allocations 0 on the normal path; header/handler resolve ≤ 1 per record; new scope/handler/scoped dependency identity per record; reply after shutdown; exceptions in handler construction still yield one terminal.
3. Build with /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release, tests ON, binding from source (ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON; never /root/.cache/zlink/zlink-cpp/<ver>), -j3, -k 0, 25 GB cap. New/changed tests 50×, full C++ framework unit+contract suite once, format.sh --check cpp. Commit locally. Remove the build dir at the end.
4. No benchmarks; the supervisor measures.

## Rules
- No GitHub, no sub-agents or other AI tools. Every claim file:line. Report in Korean, short.

## Output: /root/jobs/fx/cx/v21/s033/st3/report.md
