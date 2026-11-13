Model gpt-6.1-sol, effort high. Earlier job briefs are history, not instructions.

# Task: #3868 — cut per-message allocations and locks on the C++ Framework send path (PM-approved option 1 with three conditions)

Worktree /root/worktree/zlink-v21-s033/c3868 (branch perf/3868-cpp-send-alloc at current origin/main, includes #4302 and #4313). Commit locally; do not push.
Read first: /root/jobs/fx/cx/v21/s033/d3868/report.md, /root/jobs/fx/cx/v21/s033/d3868/site-attribution-kairos.txt (lower-bound site ranking), /root/jobs/fx/cx/v21/s033/d3868/agg.py, and the d3868 BPF script(s) under d3868/raw (count.bt etc.). Also /root/jobs/fx/cx/v21/s033/csendb/report.md: Node-record independent execution and multi-drain were tried and made it WORSE; do not repeat them.

Baseline (d3868, main c81c7b7314): 1 KiB send-saturation F÷B 0.190. Per msg: malloc source 16.1, target 22.6 (binding 7.1 / 2.4); pthread_mutex source 19.2, target 13.4 (binding 4.7 / 1.6). Total CPU 56.0 µs/msg vs budget 44.8.

## Step 0 — handler kind and spec (report table, no code)
- Bench target: framework/bench/grpc/cpp/framework/bench_framework_cpp_server.cpp registers `add_route_send_handler<command_handler_t>` on a route mesh Node (object_role none). Confirm from 06-framework-api.ko.md §10-11 (:486-530) and 08-messaging-hot-path.ko.md:253-265 which lifetime applies (Channel/route handler: a new scope + instance per dispatch, never shared; Spot/Actor: once per activation). If the C++ code creates per-message scope/instance for a Spot or Actor handler anywhere on this path, that is a spec defect: report it with file:line and fix it FIRST.
- Language comparison line (one row each, file:line): what .NET, Java, Node and C++ create per dispatch for this same handler kind (scope object, handler instance, filter list, dependency lookups, heap allocations you can see in code).

## Step 1 — per-dispatch creation cost (target)
Keep a NEW scope and handler instance per dispatch. Reduce what that creation costs: heap allocations and locks in service_scope_t::create, own_dispatch_instance, create_dispatch_instance, route_handler_invoker/route_packet_dispatcher, decode_application_parts, mesh_record_dispatcher. Allowed (spec :259-265): creation procedure decided at registration and reused; direct lookup by precomputed service key; scope storage empty/inline when the dispatch has no scoped dependencies. Not allowed: sharing instance or scoped dependency across dispatches, pools that hand an old instance to a new dispatch.

## Step 2 — topology peer lookup (source)
service_topology_registry_t::peer / peers_on_lane take a lock per message. Do NOT copy the peer at registration (a stale copy becomes second state when the descriptor changes). Make the owner publish an immutable snapshot and let the send path read it without a lock (same shape as Java peerObservation — find it in framework/languages/java and cite). Add a behavioural test: descriptor change after start is seen by the next send (fails if a stale copy is used).

## Step 3 — serializer lookup (source)
serializer_registry_t::cached_serializer takes a lock per message. If the registry cannot change after start, prove it with file:line and resolve once at route registration. Otherwise use the same published-snapshot read as Step 2.

## Measure
- For each step, before/after with the d3868 count capture: allocs/msg and mutex/msg per process. Enlarge BPF map sizes so per-site totals match the count-only totals (report both).
- Final: 1 KiB send-saturation 30 s × 3 pairs Framework vs binding, condition B (source 0-7, target 8-15), plus total CPU/msg at 80k paced (user+kernel, both processes), same scripts as d3868. Request-backpressure and request-serial 5 s × 3 too.
- ALL measurement and profiling: `flock /root/jobs/fx/cx/measure.lock` EXCLUSIVE. Build first; chain build && verify binary && measure.

## Tests
Full `ctest --preset linux-ninja-release -L '^(unit|contract)$'` once at the end; the new snapshot test 20×; `bash scripts/format/format.sh --check cpp`.

## Rules
- Compile only through /root/jobs/fx/cx/cpp-build-slot.sh (no outer flock), Release preset, ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON, Core prefix /root/.cache/zlink/core-release/1.25.0/linux-x64, -j3, build dir /root/worktree/zlink-v21-s033/c3868/build-c3868, 25 GB cap; remove at end.
- No new flags, counters, std::function or decision sites; list removed vs added decision sites.
- Do NOT run scripts/dev/internal-docs.sh, git pull/fetch/push, GitHub, sub-agents or other AI tools. Raw outputs in /root/jobs/fx/cx/v21/s033/c3868/raw; never delete.
- Korean, short tables, every number cites its raw file.

## Output
/root/jobs/fx/cx/v21/s033/c3868/report.md: Step 0 table + language row, per-step diff summary with before/after allocs/locks, final F÷B and CPU vs budget 44.8, tests, removed/added decision sites, remaining gap.
