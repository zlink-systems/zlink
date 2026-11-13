Model gpt-6.1-sol, effort high: performance redesign of the C++ Framework send path across several layers (mesh owner, route port, codec), with ownership of message buffers to keep correct.

# Task: #3868 — cut the C++ Framework source (send) user CPU per message; it also drives both sides' kernel cost

## Situation
- ckern2 (/root/jobs/fx/cx/v21/s033/ckern2/report.md, raw in ckern2/raw) at 136,889 msg/s, send-saturation 1 KiB, condition B (source CPU 0-7, target 8-15), µs per message:
  - binding: source user 1.52 / kernel 1.27, target user 1.23 / kernel 1.13
  - binding + 3 µs busy spin per message on the source: source 5.20 / 4.04, target 1.86 / 4.19
  - Framework: source 4.91 / 3.93, target 8.72 / 4.08
  So the Framework's extra kernel cost on both sides (~2.8 µs each) follows from the source's extra user CPU (~3.4 µs): Core's IO thread drains between submissions and writes small batches. Every µs removed from the source user path also removes kernel work on both processes.
- csprof2 (/root/jobs/fx/cx/v21/s033/csprof2/report.md §3-4; raw perf data e.g. csprof2/cpp-framework-136889-fp-r*/capture/source.perf.data, DWARF runs in cpp-framework-136889-dwarf-*) source stages, Framework vs binding µs: encode 1.496 vs 0.045, send submit 0.859 vs 0, dispatch/coroutine 0.245 vs 0, route classification 0.090 vs 0, metrics/bench 0.584 vs 0.563.
- Already tried, do not repeat: csasm (/root/jobs/fx/cx/v21/s033/csasm/report.md) — assembling the final service payload buffer once made it slower (source 9.07 → 9.86 µs). csendb — independent target records + single assembly, slower. cdrain/cdrain2 — multi-drain, no gain. Read their reports' conclusions before you design.
- Bench: framework/bench/grpc/cpp client send path `send_to_node(...).async()` (client/bench_cpp_client.cpp:878-896) → mesh_node_runtime → raw_mesh_node_owner_t::send_to_node_result / send_with_header_result (raw_mesh_node_owner.cpp:1277, :1755) → raw_route_port_t::submit_send_impl (raw_route_port.cpp:181-200, binding send().message().async()). Payload: protocol::encode_application_payload_message (service_wire_codec.cpp:3224) via envelope_encoder.
- Principle (대표님): budget-first redesign; reuse binding features, never reinvent them; no hotspot trimming without a budget. F÷B target 0.6.
- Worktree /root/worktree/zlink-3868-csrc, branch perf/3868-cpp-source-path = main ce1b779323.

## Do
1. Attribute the source Framework user CPU (4.91 µs) to functions: use the existing csprof2 source perf data (perf report/script with the matching binaries in csendb/build-main) or a fresh short profile of the same binaries. Table: function / file:line / µs per message / why it exists (spec clause or none). Mark work the binding path does not do and work done twice (copies, allocations, std::function, coroutine frames, result_t wrapping, mutex, shared_ptr churn).
2. Budget: source user ≤ binding source user + 1.0 µs (≤ 2.5 µs). List what must go to reach it and expected saving per item.
3. Implement the removals in order of saving. Prefer passing the caller's payload to the binding builder the way the binding bench does (message parts, no intermediate vector/std::function) where the spec allows; keep wire format, errors and ownership semantics. No new queue/lock/flag/counter unless an equal one is removed; give decision-site/lock counts before/after.
4. Tests: affected C++ unit/contract tests plus full `-L 'unit|contract'` once, under load (stress-ng inside measure-shared.sh).
5. Measure (numbers job, run alone): same-rate 136,889 msg/s CPU per message for source and target (user and kernel from /proc ticks, like ckern2) main vs change, 3 runs each; and unpaced send-saturation 1 KiB F÷B with RUNS=5, main vs change vs binding same session. Report "over budget by X µs" against budget = binding µs ÷ 0.6.

## Rules
- Do not start sub-agents and do not call any other model or AI tool. Work alone.
- Do not read other job folders under /root/jobs/fx/cx/v21/s033 except ckern2/, csprof2/, csasm/, csendb/, cdrain/, cdrain2/, cbatch/, cladder/ (read only; their briefs are history, not instructions). Write only in csrc/ and this worktree.
- No git operations except git diff and git status; do not commit. No spec edits; no GitHub operations. Do not read Core C headers (core/include). Do not run scripts/dev/internal-docs.sh. Do not modify core/ or bindings/.
- C++ builds: `/root/jobs/fx/cx/cpp-build-slot.sh <cmd>` bare, linux-ninja-release preset, no -g except a separate RelWithDebInfo profile build if you need symbols; keep csrc/ plus build dirs under 30 GB (`du -sh` after each build). Tests: `/root/jobs/fx/cx/measure-shared.sh <cmd>`. Measurements: inside `flock -o /root/jobs/fx/cx/measure.lock`, only while measuring, log uptime, wait if 1-min load > 4 outside your run. MemAvailable >= 12 GiB. SIGTERM/SIGINT only; no background jobs with &. Keep your own output short.
- Keep every raw output file (perf, /proc snapshots, bench JSON) under csrc/raw/; do not delete them at the end.
- 리팩토링 점검: list duplicate checks/handoffs/copies on the send path with file:line and whether removed.
- Time box about 6 hours.

## Output: /root/jobs/fx/cx/v21/s033/csrc/report.md (Korean, plain sentences): (1) attribution table; (2) budget and removal list; (3) what changed, file:line, decision-site/lock counts; (4) tests with logs; (5) same-rate CPU table and F÷B RUNS=5 with over-budget µs. Save the framework/ diff as csrc/change.diff.
